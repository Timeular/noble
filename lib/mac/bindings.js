const events = require('events');
const util = require('util');

const NobleMac = require('bindings')('noble').NobleMac;

util.inherits(NobleMac, events.EventEmitter);

module.exports = new NobleMac();
