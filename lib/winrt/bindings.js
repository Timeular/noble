const events = require('events');
const util = require('util');

const NobleWinrt =  require('bindings')('noble').NobleWinrt;

util.inherits(NobleWinrt, events.EventEmitter);

module.exports = new NobleWinrt();
