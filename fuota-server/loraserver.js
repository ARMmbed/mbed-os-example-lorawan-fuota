/**
 * Experimental FUOTA server for loraserver.io
 *
 * Note that this is completely hacked together, and I'd suggest to never
 */

const LORASERVER_HOST = process.env.LORA_HOST || '192.168.122.132';
const PACKET_FILE = process.argv[2];
const DATARATE = process.env.LORA_DR || 0;

if (!PACKET_FILE) throw 'Syntax: loraserver.io PACKET_FILE'

const LORASERVER_API = 'https://' + LORASERVER_HOST + ':8080';
const LORASERVER_MQTT = 'mqtt://' + LORASERVER_HOST + ':1883';

const mqtt = require('mqtt')
const client = mqtt.connect(LORASERVER_MQTT);
const gpsTime = require('gps-time');
const fs = require('fs');
const rp = require('request-promise');

const CLASS_C_WAIT_S = 15;

// all devices that you want to update
const devices = [
    // '00a99d4921b26d75'
    '00800000040004c9'
];

// details for the multicast group
const mcDetails = {
    applicationID: '1',
    devEUI: '00a99d4921b26d76',
};

let classCStarted = false;

let deviceMap = devices.reduce((curr, eui) => {
    curr[eui] = { clockSynced: false, fragSessionAns: false, mcSetupAns: false, mcStartAns: false, applicationID: null, msgWaiting: null };
    return curr;
}, {});

let startTime = null;

process.env["NODE_TLS_REJECT_UNAUTHORIZED"] = 0;

client.on('connect', function () {
    client.subscribe('application/#', function (err) {
        if (err) {
            return console.error('Failed to subscribe', err);
        }

        console.log('Subscribed to all application events');
    });
})

client.on('message', async function (topic, message) {
    console.log('msg', message.toString('utf-8'));

    // only interested in rx for now...
    if (!/\/rx$/.test(topic)) return;


    // message is Buffer
    let m = JSON.parse(message.toString('utf-8'));
    // console.log('Rx', topic, m);

    // device that we don't care about
    if (!deviceMap[m.devEUI]) {
        return;
    }

    if (m.fPort === 202 /* clock sync */) {
        let body = Buffer.from(m.data, 'base64');
        if (body[0] === 0x1 /* CLOCK_APP_TIME_REQ */) {
            let deviceTime = body[1] + (body[2] << 8) + (body[3] << 16) + (body[4] << 24);
            let serverTime = gpsTime.toGPSMS(Date.now()) / 1000 | 0;
            console.log('deviceTime', deviceTime, 'serverTime', serverTime);

            let adjust = serverTime - deviceTime | 0;
            let resp = [ 1, adjust & 0xff, (adjust >> 8) & 0xff, (adjust >> 16) & 0xff, (adjust >> 24) & 0xff, 0b0000 /* tokenAns */ ];

            let responseMessage = {
                "reference": "jan" + Date.now(),
                "confirmed": false,
                "fPort": 202,
                "data": Buffer.from(resp).toString('base64')
            };

            deviceMap[m.devEUI].msgWaiting = responseMessage;

            deviceMap[m.devEUI].clockSynced = true;
            deviceMap[m.devEUI].applicationID = m.applicationID;

            console.log('Clock sync for device', m.devEUI, adjust, 'seconds');

            if (devices.every(eui => deviceMap[eui].clockSynced)) {
                console.log('All devices have had their clocks synced, setting up mc group...');
                setTimeout(sendMcGroupSetup, 1000);
            }
        }
        else {
            console.warn('Could not handle clock sync request', body);
        }
    }

    if (m.fPort === 200 /* mc group cmnds */) {
        let body = Buffer.from(m.data, 'base64');
        if (body[0] === 0x2) { // McGroupSetupAns
            if (body[1] === 0x0) {
                deviceMap[m.devEUI].mcSetupAns = true;
            }
            else {
                console.warn('Unexpected answer for McGroupSetupAns from', m.devEUI, body)
            }

            if (devices.every(eui => deviceMap[eui].mcSetupAns)) {
                console.log('All devices have received multicast group, setting up fragsession...');
                setTimeout(sendFragSessionSetup, 1000);
            }
        }
        else if (body[0] === 0x4) { // McClassCSessionAns
            if (body[1] !== 0x0) return console.warn('Unexpected byte[1] for McClassCSessionAns', m.devEUI, body);

            let tts = body[2] + (body[3] << 8) + (body[4] << 16);
            console.log(m.devEUI, 'time to start', tts, 'startTime is', startTime, 'currtime is', gpsTime.toGPSMS(Date.now()) / 1000 | 0);

            deviceMap[m.devEUI].mcStartAns = true;

            // so this app cannot properly check the delta, as we don't know when the network is gonna send
            // should be calculated at that very moment, so now there can be a few seconds delay
            let delta = (gpsTime.toGPSMS(Date.now()) / 1000 | 0) + tts - startTime;
            if (Math.abs(delta) > 6) {
                console.log('Delta is too big for', m.devEUI, Math.abs(delta));
            }
            else {
                console.log('Delta is OK', m.devEUI, delta);
            }
        }
        else {
            console.warn('Could not handle Mc Group command', body);
        }
    }

    if (m.fPort === 201 /* frag session */) {
        let body = Buffer.from(m.data, 'base64');
        if (body[0] === 0x2) { // FragSessionSetupAns
            if (body[1] === 0x0) {
                deviceMap[m.devEUI].fragSessionAns = true;
            }
            else {
                console.warn('Unexpected answer for FragSessionSetupAns from', m.devEUI, body)
            }

            if (devices.every(eui => deviceMap[eui].fragSessionAns)) {
                console.log('All devices have received frag session, sending mc start msg...');
                setTimeout(sendMcClassCSessionReq, 1000);
            }
        }
        else if (body[0] === 0x5) { // DATA_BLOCK_AUTH_REQ
            let hash = '';
            for (let ix = 5; ix > 1; ix--) {
                hash += body.slice(ix, ix+1).toString('hex');
            }
            console.log('Received DATA_BLOCK_AUTH_REQ', m.devEUI, hash);
        }
        else {
            console.warn('Could not handle Mc Group command', body);
        }
    }

    if (deviceMap[m.devEUI].msgWaiting) {
        let msgWaiting = deviceMap[m.devEUI].msgWaiting;
        client.publish(`application/${m.applicationID}/device/${m.devEUI}/tx`, Buffer.from(JSON.stringify(msgWaiting), 'utf8'));
        deviceMap[m.devEUI].msgWaiting = null;
    }
});

function sendMcGroupSetup() {
    if (classCStarted) return;

    console.log('sendMcGroupSetup');
    // mcgroupsetup
    let mcGroupSetup = {
        "reference": "jan" + Date.now(),
        "confirmed": false,
        "fPort": 200,
        "data": Buffer.from([ 0x02, 0x00,
            0xFF, 0xFF, 0xFF, 0x01, // McAddr
            0x01, 0x5E, 0x85, 0xF4, 0xB9, 0x9D, 0xC0, 0xB9, 0x44, 0x06, 0x6C, 0xD0, 0x74, 0x98, 0x33, 0x0B, //McKey_encrypted
            0x0, 0x0, 0x0, 0x0, // minFCnt
            0xff, 0xff, 0x0, 0x0 // maxFCnt
        ]).toString('base64')
    };

    devices.forEach(eui => {
        let dm = deviceMap[eui];
        if (dm.mcSetupAns) return;

        dm.msgWaiting = mcGroupSetup;
    });

    // retry
    setTimeout(() => {
        if (devices.some(eui => !deviceMap[eui].mcSetupAns)) {
            sendMcGroupSetup();
        }
    }, 20000);
}

function sendFragSessionSetup() {
    if (classCStarted) return;

    console.log('sendFragSessionSetup');
    let msg = {
        "reference": "jan" + Date.now(),
        "confirmed": false,
        "fPort": 201,
        "data": Buffer.from(parsePackets()[0]).toString('base64')
    };

    devices.forEach(eui => {
        let dm = deviceMap[eui];
        if (dm.fragSessionAns) return;

        dm.msgWaiting = msg;
    });

    // retry
    setTimeout(() => {
        if (devices.some(eui => !deviceMap[eui].fragSessionAns)) {
            sendFragSessionSetup();
        }
    }, 20000);
}

function sendMcClassCSessionReq() {
    if (classCStarted) return;

    console.log('sendMcClassCSessionReq');

    if (!startTime) {
        let serverTime = gpsTime.toGPSMS(Date.now()) / 1000 | 0;
        startTime = serverTime + CLASS_C_WAIT_S; // 60 seconds from now

        setTimeout(() => {
            startSendingClassCPackets();
        }, (CLASS_C_WAIT_S + 10) * 1000); // because the delta drift that we don't know (see above)
    }

    let msg = {
        "reference": "jan" + Date.now(),
        "confirmed": false,
        "fPort": 200,
        "data": Buffer.from([
            0x4,
            0x0, // mcgroupidheader
            startTime & 0xff, (startTime >> 8) & 0xff, (startTime >> 16) & 0xff, (startTime >> 24) & 0xff,
            0x08, // session timeout pow(2,8) = 256. Up this if you need more time.
            0xd2, 0xad, 0x84, // dlfreq
            DATARATE // dr
        ]).toString('base64')
    };

    devices.forEach(eui => {
        let dm = deviceMap[eui];
        if (dm.mcStartAns) return;

        dm.msgWaiting = msg;
    });

    // retry
    setTimeout(() => {
        if (devices.some(eui => !deviceMap[eui].mcStartAns)) {
            sendMcClassCSessionReq();
        }
    }, 20000);
}

function sleep(ms) {
    return new Promise((res, rej) => setTimeout(res, ms));
}

function parsePackets() {
    let packets = fs.readFileSync(PACKET_FILE, 'utf-8').split('\n').map(row => {
        return row.split(' ').map(c=>parseInt(c, 16))
    });
    return packets;
}

async function startSendingClassCPackets() {
    classCStarted = true;
    console.log('startSendingClassCPackets');
    console.log('All devices ready?', deviceMap);

    let packets = parsePackets();

    let counter = 0;

    for (let p of packets) {
        // first row is header, don't use that one
        if (counter === 0) {
            counter++;
            continue;
        }

        let msg = {
            "reference": "jan" + Date.now(),
            "confirmed": false,
            "fPort": 201,
            "data": Buffer.from(p).toString('base64')
        };

        client.publish(`application/${mcDetails.applicationID}/device/${mcDetails.devEUI}/tx`, Buffer.from(JSON.stringify(msg), 'utf8'));

        console.log('Sent packet', ++counter);

        await sleep(2200); // tpacket on SF12 is 2100 ms. so this should just work (although loraserver doesn't think it's a problem to send faster)
    }

    console.log('Done sending all packets');
}

client.on('error', err => console.error('Error on MQTT subscriber', err));
