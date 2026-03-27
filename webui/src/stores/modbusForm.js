import { writable } from 'svelte/store';

export const modbusSlaveAddr = writable(1);
export const modbusBaudRate = writable(19200);
export const modbusParity = writable('even');
