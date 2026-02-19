import fs from 'fs';
import path from 'path';
import { getConfigLoader, ConfigLoader } from './config.js';
import { createGatewayServer, GatewayServer } from './gateway/server.js';
import { registerApiRoutes } from './gateway/routes/api.js';
import { registerWebSocketRoutes, WebSocketHandler } from './gateway/websocket.js';
import { registerChatRoutes } from './gateway/chat.js';
import { getNodeManager, NodeManager } from './nodes/manager.js';
import { getAgent, CiraAgent } from './agent/agent.js';
import { createLineChannel, LineChannel } from './channels/line.js';
import { createMqttChannel, MqttChannel } from './channels/mqtt.js';
import { createStatsCollector, StatsCollector } from './services/stats-collector.js';
import { createModbusServer, ModbusServer } from './services/modbus-server.js';
import { createLogger, logger as rootLogger } from './utils/logger.js';
import { CiraConfig } from './utils/config-schema.js';

const logger = createLogger('main');

// Global state for graceful shutdown
let configLoader: ConfigLoader;
let nodeManager: NodeManager;
let gateway: GatewayServer;
let wsHandler: WebSocketHandler;
let agent: CiraAgent;
let lineChannel: LineChannel | null = null;
let mqttChannel: MqttChannel | null = null;
let statsCollector: StatsCollector | null = null;
let modbusServer: ModbusServer | null = null;
let config: CiraConfig;
let configPath: string | undefined;
let isShuttingDown = false;

/**
 * Graceful shutdown handler
 * Closes all connections and cleans up resources
 */
async function gracefulShutdown(signal: string): Promise<void> {
  if (isShuttingDown) {
    logger.warn('Shutdown already in progress, ignoring duplicate signal');
    return;
  }

  isShuttingDown = true;
  logger.info(`Received ${signal}, initiating graceful shutdown...`);

  const shutdownTimeout = setTimeout(() => {
    logger.error('Shutdown timeout exceeded, forcing exit');
    process.exit(1);
  }, 30000); // 30 second timeout

  try {
    // Stop stats collector
    if (statsCollector) {
      logger.debug('Stopping stats collector...');
      statsCollector.stop();
    }

    // Stop MODBUS server
    if (modbusServer) {
      logger.debug('Stopping MODBUS server...');
      await modbusServer.stop();
    }

    // Stop accepting new connections
    logger.debug('Stopping health checks...');
    nodeManager?.stopHealthChecks();

    // Disconnect MQTT channel
    if (mqttChannel?.isConnected()) {
      logger.debug('Disconnecting MQTT...');
      await mqttChannel.disconnect();
    }

    // Close HTTP/WebSocket server
    logger.debug('Closing gateway server...');
    if (gateway) {
      await gateway.stop();
    }

    // Flush logs (give pino time to flush)
    logger.info('Shutdown complete');
    rootLogger.flush();

    clearTimeout(shutdownTimeout);

    // Small delay to ensure logs are flushed
    await new Promise(resolve => setTimeout(resolve, 100));

    process.exit(0);
  } catch (error) {
    logger.error(`Error during shutdown: ${error}`);
    clearTimeout(shutdownTimeout);
    process.exit(1);
  }
}

/**
 * Reload configuration on SIGHUP
 */
async function reloadConfig(): Promise<void> {
  logger.info('Received SIGHUP, reloading configuration...');

  try {
    const newConfig = await configLoader.load(configPath);
    config = newConfig;

    // Reload node configurations
    await configLoader.loadNodes();

    logger.info('Configuration reloaded successfully');
    logger.info(`Gateway name: ${config.gateway.name}`);
  } catch (error) {
    logger.error(`Failed to reload configuration: ${error}`);
    // Keep running with old configuration
  }
}

/**
 * Setup signal handlers for daemon operation
 */
function setupSignalHandlers(): void {
  // Graceful shutdown on SIGTERM (systemd stop) and SIGINT (Ctrl+C)
  process.on('SIGTERM', () => gracefulShutdown('SIGTERM'));
  process.on('SIGINT', () => gracefulShutdown('SIGINT'));

  // Reload configuration on SIGHUP
  process.on('SIGHUP', () => {
    reloadConfig().catch(err => {
      logger.error(`Config reload failed: ${err}`);
    });
  });
}

/**
 * Setup error handlers for daemon operation
 */
function setupErrorHandlers(): void {
  // Handle uncaught exceptions - log and exit so systemd can restart
  process.on('uncaughtException', (error: Error) => {
    logger.fatal({ err: error }, 'Uncaught exception, exiting for restart');
    rootLogger.flush();

    // Give logs time to flush before exit
    setTimeout(() => {
      process.exit(1);
    }, 100);
  });

  // Handle unhandled promise rejections - log and exit so systemd can restart
  process.on('unhandledRejection', (reason: unknown) => {
    logger.fatal({ reason }, 'Unhandled promise rejection, exiting for restart');
    rootLogger.flush();

    // Give logs time to flush before exit
    setTimeout(() => {
      process.exit(1);
    }, 100);
  });
}

/**
 * Parse command line arguments
 */
function parseArgs(): void {
  const args = process.argv.slice(2);

  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--config' && args[i + 1]) {
      configPath = args[i + 1];
      i++;
    } else if (args[i] === '--help' || args[i] === '-h') {
      console.log(`
CiRA CLAW

Usage: cira-claw [options]

Options:
  --config <path>  Path to configuration file (default: ~/.cira/cira.json)
  --help, -h       Show this help message

Environment Variables:
  CIRA_HOME        Configuration directory (default: ~/.cira)
  NODE_ENV         Set to 'production' for production logging
  LOG_LEVEL        Log level: debug, info, warn, error, fatal
  ANTHROPIC_API_KEY  Claude API key for AI agent
`);
      process.exit(0);
    }
  }
}

/**
 * Main entry point
 */
async function main(): Promise<void> {
  // Setup error handlers first
  setupErrorHandlers();

  logger.info('Starting CiRA CLAW...');
  logger.info(`Node.js ${process.version}, PID ${process.pid}`);

  // Parse command line arguments
  parseArgs();

  try {
    // Initialize configuration
    configLoader = getConfigLoader();
    await configLoader.init();
    config = await configLoader.load(configPath);

    logger.info(`Configuration loaded: ${config.gateway.name}`);

    // Initialize node manager
    nodeManager = getNodeManager(configLoader);
    await nodeManager.init();

    // Create and configure gateway server
    gateway = await createGatewayServer(config);

    // Register API routes
    await registerApiRoutes(gateway.fastify, nodeManager);

    // Register WebSocket handler for real-time node data
    wsHandler = await registerWebSocketRoutes(gateway.fastify, nodeManager);

    // Initialize AI agent
    agent = getAgent(config.agent, configLoader.workspacePath);
    await agent.init();

    if (agent.isAvailable()) {
      logger.info('AI agent initialized');
    } else {
      logger.warn('AI agent not available - no API key configured');
    }

    // Initialize LINE channel if enabled
    if (config.channels.line.enabled) {
      const lineCredentials = await loadLineCredentials(configLoader.credentialsPath);
      if (lineCredentials) {
        lineChannel = createLineChannel(lineCredentials, agent, nodeManager);
        await lineChannel.register(gateway.fastify);
        logger.info('LINE channel registered');
      } else {
        logger.warn('LINE channel enabled but credentials not found');
      }
    }

    // Initialize MQTT channel if enabled
    if (config.channels.mqtt.enabled) {
      mqttChannel = createMqttChannel(
        {
          broker: config.channels.mqtt.broker,
          topics: config.channels.mqtt.topics,
        },
        agent,
        nodeManager
      );

      try {
        await mqttChannel.connect();
        logger.info('MQTT channel connected');
      } catch (error) {
        logger.warn(`Failed to connect MQTT: ${error}`);
        mqttChannel = null;
      }
    }

    // Initialize stats collector for data accumulation and MQTT publishing
    const statsDataDir = path.join(configLoader.workspacePath, 'stats');
    statsCollector = createStatsCollector(
      nodeManager,
      mqttChannel,
      config.alerts,
      statsDataDir
    );
    statsCollector.start(10000); // Poll every 10 seconds
    logger.info('Stats collector started');

    // Register WebChat routes for agent chat (after statsCollector for real data)
    await registerChatRoutes(gateway.fastify, {
      agent,
      nodeManager,
      statsCollector,
      alertsConfig: config.alerts,
    });

    // Initialize MODBUS server if enabled
    if (config.channels.modbus.enabled) {
      modbusServer = createModbusServer(
        {
          port: config.channels.modbus.port,
          host: config.channels.modbus.host,
        },
        nodeManager,
        statsCollector
      );

      try {
        await modbusServer.start();
        logger.info(`MODBUS server started on port ${config.channels.modbus.port}`);
      } catch (error) {
        logger.warn(`Failed to start MODBUS server: ${error}`);
        modbusServer = null;
      }
    }

    // Start health checks
    nodeManager.startHealthChecks(30000); // Every 30 seconds

    // Setup signal handlers before starting server
    setupSignalHandlers();

    // Start the server
    await gateway.start();

    const host = config.gateway.host === '0.0.0.0' ? 'localhost' : config.gateway.host;
    const port = config.gateway.port;

    logger.info('CiRA CLAW started successfully');
    logger.info(`  Web Dashboard: http://${host}:${port}`);
    logger.info(`  API: http://${host}:${port}/api`);
    logger.info(`  WebSocket: ws://${host}:${port}/ws`);
    logger.info(`  Chat: ws://${host}:${port}/chat`);
    logger.info(`  Health: http://${host}:${port}/health`);
    if (modbusServer) {
      logger.info(`  MODBUS: tcp://${host}:${config.channels.modbus.port}`);
    }

    // Log ready status for systemd
    if (process.env.NODE_ENV === 'production') {
      logger.info('Gateway ready and accepting connections');
    }

  } catch (error) {
    logger.fatal({ err: error }, 'Failed to start gateway');
    process.exit(1);
  }
}

/**
 * Load LINE channel credentials from credentials directory
 */
async function loadLineCredentials(credentialsPath: string): Promise<{ channelAccessToken: string; channelSecret: string } | null> {
  const lineCredPath = path.join(credentialsPath, 'line.json');

  if (!fs.existsSync(lineCredPath)) {
    return null;
  }

  try {
    const content = fs.readFileSync(lineCredPath, 'utf-8');
    const creds = JSON.parse(content) as { channel_access_token?: string; channel_secret?: string };

    if (!creds.channel_access_token || !creds.channel_secret) {
      logger.warn('LINE credentials file missing required fields');
      return null;
    }

    return {
      channelAccessToken: creds.channel_access_token,
      channelSecret: creds.channel_secret,
    };
  } catch (error) {
    logger.error(`Failed to load LINE credentials: ${error}`);
    return null;
  }
}

main();
