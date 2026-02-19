#!/usr/bin/env node
/**
 * Simple MODBUS TCP client test for CiRA CLAW
 * Tests reading registers from the MODBUS server
 */

import net from 'net';

const HOST = '127.0.0.1';
const PORT = 1502;

// Build MODBUS TCP request to read holding registers
function buildReadRequest(startAddr, quantity, transactionId = 1) {
  const buffer = Buffer.alloc(12);

  // MBAP Header
  buffer.writeUInt16BE(transactionId, 0);  // Transaction ID
  buffer.writeUInt16BE(0, 2);               // Protocol ID (0 = MODBUS)
  buffer.writeUInt16BE(6, 4);               // Length (6 bytes follow)
  buffer.writeUInt8(1, 6);                  // Unit ID

  // PDU
  buffer.writeUInt8(0x03, 7);               // Function code: Read Holding Registers
  buffer.writeUInt16BE(startAddr, 8);       // Starting address
  buffer.writeUInt16BE(quantity, 10);       // Quantity of registers

  return buffer;
}

// Parse register values from response
function parseResponse(data) {
  if (data.length < 9) {
    return { error: 'Response too short' };
  }

  const transactionId = data.readUInt16BE(0);
  const functionCode = data.readUInt8(7);

  // Check for exception
  if (functionCode & 0x80) {
    const exceptionCode = data.readUInt8(8);
    return { error: `Exception ${exceptionCode}`, transactionId };
  }

  const byteCount = data.readUInt8(8);
  const registers = [];

  for (let i = 0; i < byteCount / 2; i++) {
    registers.push(data.readUInt16BE(9 + i * 2));
  }

  return { transactionId, functionCode, registers };
}

// Combine two 16-bit registers into 32-bit value
function toUInt32(low, high) {
  return (high << 16) | low;
}

async function testModbus() {
  console.log(`\n=== CiRA CLAW MODBUS Test ===`);
  console.log(`Connecting to ${HOST}:${PORT}...\n`);

  return new Promise((resolve, reject) => {
    const client = net.createConnection({ host: HOST, port: PORT }, () => {
      console.log('Connected to MODBUS server\n');

      let transactionId = 1;

      // Test 1: Read node 0 registers (0-9)
      console.log('--- Reading Node 0 Registers (0-9) ---');
      const req1 = buildReadRequest(0, 10, transactionId++);
      client.write(req1);
    });

    let testPhase = 0;
    let transactionId = 2;

    client.on('data', (data) => {
      const result = parseResponse(data);

      if (result.error) {
        console.log(`Error: ${result.error}`);
      } else {
        const regs = result.registers;

        if (testPhase === 0) {
          // Node 0 registers
          console.log('Register values:');
          console.log(`  [0-1] Total Detections: ${toUInt32(regs[0], regs[1])}`);
          console.log(`  [2-3] Total Frames:     ${toUInt32(regs[2], regs[3])}`);
          console.log(`  [4]   FPS x10:          ${regs[4]} (${(regs[4]/10).toFixed(1)} fps)`);
          console.log(`  [5-6] Uptime (sec):     ${toUInt32(regs[5], regs[6])}`);
          console.log(`  [7]   Status:           ${regs[7]} (0=unknown, 1=online, 2=offline, 3=error)`);
          console.log(`  [8-9] Reserved:         ${regs[8]}, ${regs[9]}`);
          console.log('');

          // Test 2: Read special registers (100-104)
          testPhase = 1;
          console.log('--- Reading Gateway Summary Registers (100-104) ---');
          const req2 = buildReadRequest(100, 5, transactionId++);
          client.write(req2);

        } else if (testPhase === 1) {
          // Gateway summary registers
          console.log('Gateway summary:');
          console.log(`  [100] Gateway Running:  ${regs[0]}`);
          console.log(`  [101] Online Nodes:     ${regs[1]}`);
          console.log(`  [102] Offline Nodes:    ${regs[2]}`);
          console.log(`  [103] Error Nodes:      ${regs[3]}`);
          console.log(`  [104] Total Nodes:      ${regs[4]}`);
          console.log('');

          console.log('=== MODBUS Test Complete ===\n');
          client.end();
          resolve();
        }
      }
    });

    client.on('error', (err) => {
      console.error(`Connection error: ${err.message}`);
      console.log('\nMake sure:');
      console.log('  1. Gateway is running (npm run dev)');
      console.log('  2. MODBUS is enabled in ~/.cira/cira.json');
      console.log(`  3. Port ${PORT} is correct\n`);
      reject(err);
    });

    client.on('close', () => {
      console.log('Connection closed');
    });

    // Timeout after 5 seconds
    setTimeout(() => {
      client.destroy();
      reject(new Error('Test timeout'));
    }, 5000);
  });
}

testModbus().catch(() => process.exit(1));
