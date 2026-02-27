import net from 'net';
import { NodeManager } from '../nodes/manager.js';
import { StatsCollector } from './stats-collector.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('modbus-server');

/**
 * MODBUS TCP Server for PLC integration
 *
 * Register Map (per node, 10 registers each):
 * Offset 0: Total detections (low word)
 * Offset 1: Total detections (high word)
 * Offset 2: Total frames (low word)
 * Offset 3: Total frames (high word)
 * Offset 4: FPS x 10 (e.g., 15.5 fps = 155)
 * Offset 5: Uptime seconds (low word)
 * Offset 6: Uptime seconds (high word)
 * Offset 7: Status (0=unknown, 1=online, 2=offline, 3=error)
 * Offset 8: Last detection count (since last poll)
 * Offset 9: Reserved
 *
 * Node addresses: Node 0 = registers 0-9, Node 1 = 10-19, etc.
 * Max 10 nodes = 100 registers
 *
 * Special registers (100-109):
 * 100: Gateway status (1=running)
 * 101: Total online nodes
 * 102: Total offline nodes
 * 103: Total error nodes
 */

export interface ModbusConfig {
  port: number;
  host?: string;
}

// MODBUS function codes
const FC_READ_HOLDING_REGISTERS = 0x03;
const FC_READ_INPUT_REGISTERS = 0x04;
const FC_WRITE_SINGLE_REGISTER = 0x06;
const FC_WRITE_MULTIPLE_REGISTERS = 0x10;

// MODBUS exception codes
const EX_ILLEGAL_FUNCTION = 0x01;
const EX_ILLEGAL_DATA_ADDRESS = 0x02;
const EX_ILLEGAL_DATA_VALUE = 0x03;

const REGISTERS_PER_NODE = 10;
const MAX_NODES = 10;
const SPECIAL_REGISTERS_START = 100;

export class ModbusServer {
  private server: net.Server | null = null;
  private nodeManager: NodeManager;
  private statsCollector: StatsCollector | null;
  private config: ModbusConfig;

  // Register data (16-bit unsigned integers)
  private registers: Uint16Array;

  // Node ID to register offset mapping
  private nodeIndexMap: Map<string, number> = new Map();

  private updateInterval: NodeJS.Timeout | null = null;

  constructor(
    config: ModbusConfig,
    nodeManager: NodeManager,
    statsCollector: StatsCollector | null
  ) {
    this.config = config;
    this.nodeManager = nodeManager;
    this.statsCollector = statsCollector;

    // Allocate register space (10 nodes * 10 registers + 10 special registers)
    this.registers = new Uint16Array(MAX_NODES * REGISTERS_PER_NODE + 10);
  }

  // Start the MODBUS server
  async start(): Promise<void> {
    // Build node index map
    const nodes = this.nodeManager.getAllNodes();
    nodes.slice(0, MAX_NODES).forEach((node, index) => {
      this.nodeIndexMap.set(node.id, index);
    });

    // Start periodic register updates
    this.updateInterval = setInterval(() => {
      this.updateRegisters();
    }, 1000); // Update every second

    // Initial update
    this.updateRegisters();

    // Create TCP server
    this.server = net.createServer((socket) => {
      this.handleConnection(socket);
    });

    return new Promise((resolve, reject) => {
      this.server!.on('error', (err) => {
        logger.error(`MODBUS server error: ${err}`);
        reject(err);
      });

      this.server!.listen(this.config.port, this.config.host || '0.0.0.0', () => {
        logger.info(`MODBUS TCP server listening on port ${this.config.port}`);
        resolve();
      });
    });
  }

  // Stop the server
  async stop(): Promise<void> {
    if (this.updateInterval) {
      clearInterval(this.updateInterval);
      this.updateInterval = null;
    }

    return new Promise((resolve) => {
      if (this.server) {
        this.server.close(() => {
          logger.info('MODBUS server stopped');
          resolve();
        });
      } else {
        resolve();
      }
    });
  }

  // Handle incoming connection
  private handleConnection(socket: net.Socket): void {
    const clientAddr = `${socket.remoteAddress}:${socket.remotePort}`;
    logger.debug(`MODBUS client connected: ${clientAddr}`);

    socket.on('data', (data) => {
      try {
        const response = this.handleRequest(data);
        socket.write(response);
      } catch (error) {
        logger.error(`MODBUS request error: ${error}`);
      }
    });

    socket.on('close', () => {
      logger.debug(`MODBUS client disconnected: ${clientAddr}`);
    });

    socket.on('error', (err) => {
      logger.debug(`MODBUS socket error: ${err.message}`);
    });
  }

  // Handle MODBUS TCP request
  private handleRequest(data: Buffer): Buffer {
    if (data.length < 12) {
      return Buffer.alloc(0);
    }

    // MODBUS TCP header
    const transactionId = data.readUInt16BE(0);
    const protocolId = data.readUInt16BE(2);
    const unitId = data.readUInt8(6);
    const functionCode = data.readUInt8(7);

    // Validate protocol ID (should be 0 for MODBUS)
    if (protocolId !== 0) {
      return Buffer.alloc(0);
    }

    let pdu: Buffer;

    switch (functionCode) {
      case FC_READ_HOLDING_REGISTERS:
      case FC_READ_INPUT_REGISTERS:
        pdu = this.handleReadRegisters(data.subarray(8));
        break;

      case FC_WRITE_SINGLE_REGISTER:
        pdu = this.handleWriteSingleRegister(data.subarray(8));
        break;

      case FC_WRITE_MULTIPLE_REGISTERS:
        pdu = this.handleWriteMultipleRegisters(data.subarray(8));
        break;

      default:
        // Illegal function exception
        pdu = Buffer.from([functionCode | 0x80, EX_ILLEGAL_FUNCTION]);
    }

    // Build response
    const responseLength = pdu.length + 1; // PDU + unit ID
    const response = Buffer.alloc(7 + pdu.length);

    response.writeUInt16BE(transactionId, 0);
    response.writeUInt16BE(0, 2); // Protocol ID
    response.writeUInt16BE(responseLength, 4);
    response.writeUInt8(unitId, 6);
    pdu.copy(response, 7);

    return response;
  }

  // Handle read holding/input registers (FC 03/04)
  private handleReadRegisters(pdu: Buffer): Buffer {
    const startAddress = pdu.readUInt16BE(0);
    const quantity = pdu.readUInt16BE(2);

    // Validate address range
    if (startAddress + quantity > this.registers.length) {
      return Buffer.from([FC_READ_HOLDING_REGISTERS | 0x80, EX_ILLEGAL_DATA_ADDRESS]);
    }

    // Validate quantity
    if (quantity < 1 || quantity > 125) {
      return Buffer.from([FC_READ_HOLDING_REGISTERS | 0x80, EX_ILLEGAL_DATA_VALUE]);
    }

    // Build response
    const byteCount = quantity * 2;
    const response = Buffer.alloc(2 + byteCount);
    response.writeUInt8(FC_READ_HOLDING_REGISTERS, 0);
    response.writeUInt8(byteCount, 1);

    for (let i = 0; i < quantity; i++) {
      response.writeUInt16BE(this.registers[startAddress + i], 2 + i * 2);
    }

    return response;
  }

  // Handle write single register (FC 06)
  private handleWriteSingleRegister(pdu: Buffer): Buffer {
    const address = pdu.readUInt16BE(0);
    const value = pdu.readUInt16BE(2);

    // Only allow writing to special control registers if needed
    // For now, we don't support writes
    return Buffer.from([FC_WRITE_SINGLE_REGISTER | 0x80, EX_ILLEGAL_DATA_ADDRESS]);
  }

  // Handle write multiple registers (FC 16)
  private handleWriteMultipleRegisters(pdu: Buffer): Buffer {
    // For now, we don't support writes
    return Buffer.from([FC_WRITE_MULTIPLE_REGISTERS | 0x80, EX_ILLEGAL_DATA_ADDRESS]);
  }

  // Update registers with current data
  private updateRegisters(): void {
    const nodes = this.nodeManager.getAllNodes();

    // Update node registers
    for (const node of nodes) {
      const index = this.nodeIndexMap.get(node.id);
      if (index === undefined) continue;

      const baseAddr = index * REGISTERS_PER_NODE;
      const status = this.nodeManager.getNodeStatus(node.id);
      const stats = this.statsCollector?.getCurrentStats(node.id);

      // Total detections (32-bit split into two 16-bit registers)
      const totalDetections = stats?.totalDetections || 0;
      this.registers[baseAddr + 0] = totalDetections & 0xFFFF; // Low word
      this.registers[baseAddr + 1] = (totalDetections >> 16) & 0xFFFF; // High word

      // Total frames (32-bit)
      const totalFrames = stats?.totalFrames || 0;
      this.registers[baseAddr + 2] = totalFrames & 0xFFFF;
      this.registers[baseAddr + 3] = (totalFrames >> 16) & 0xFFFF;

      // FPS x 10
      const fps = stats?.fps || status?.metrics.fps || 0;
      this.registers[baseAddr + 4] = Math.round(fps * 10);

      // Uptime seconds (32-bit)
      const uptime = stats?.uptimeSec || status?.metrics.uptime || 0;
      this.registers[baseAddr + 5] = uptime & 0xFFFF;
      this.registers[baseAddr + 6] = (uptime >> 16) & 0xFFFF;

      // Status code
      let statusCode = 0;
      switch (status?.status) {
        case 'online':
          statusCode = 1;
          break;
        case 'offline':
          statusCode = 2;
          break;
        case 'error':
          statusCode = 3;
          break;
        default:
          statusCode = 0;
      }
      this.registers[baseAddr + 7] = statusCode;

      // Reserved
      this.registers[baseAddr + 8] = 0;
      this.registers[baseAddr + 9] = 0;
    }

    // Update special registers
    const summary = this.nodeManager.getSummary();
    this.registers[SPECIAL_REGISTERS_START + 0] = 1; // Gateway running
    this.registers[SPECIAL_REGISTERS_START + 1] = summary.online;
    this.registers[SPECIAL_REGISTERS_START + 2] = summary.offline;
    this.registers[SPECIAL_REGISTERS_START + 3] = summary.error;
    this.registers[SPECIAL_REGISTERS_START + 4] = summary.total;
  }

  // Get current register values (for debugging)
  getRegisters(): Uint16Array {
    return this.registers;
  }

  // Get the register address for a node
  getNodeBaseAddress(nodeId: string): number | undefined {
    const index = this.nodeIndexMap.get(nodeId);
    if (index === undefined) return undefined;
    return index * REGISTERS_PER_NODE;
  }

  /**
   * Set a holding register value programmatically.
   * Used by ActionRunner for rule-triggered MODBUS writes.
   */
  setHoldingRegister(address: number, value: number): void {
    if (address < 0 || address >= this.registers.length) {
      throw new Error(`Invalid register address: ${address}`);
    }
    if (value < 0 || value > 65535) {
      throw new Error(`Invalid register value: ${value} (must be 0-65535)`);
    }
    this.registers[address] = value;
    logger.debug(`Register ${address} set to ${value}`);
  }
}

export function createModbusServer(
  config: ModbusConfig,
  nodeManager: NodeManager,
  statsCollector: StatsCollector | null
): ModbusServer {
  return new ModbusServer(config, nodeManager, statsCollector);
}
