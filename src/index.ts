import 'dotenv/config';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { getConfigLoader, ConfigLoader } from './config.js';
import { createGatewayServer, GatewayServer } from './gateway/server.js';
import { registerApiRoutes, setRuleEngine, setStatsCollector, setSignalBridge } from './gateway/routes/api.js';
import { registerWebSocketRoutes, WebSocketHandler } from './gateway/websocket.js';
import { registerChatRoutes } from './gateway/chat.js';
import { getNodeManager, NodeManager } from './nodes/manager.js';
import { getAgent, CiraAgent } from './agent/agent.js';
import { createLineChannel, LineChannel } from './channels/line.js';
import { createMqttChannel, MqttChannel } from './channels/mqtt.js';
import { createStatsCollector, StatsCollector } from './services/stats-collector.js';
import { createModbusServer, ModbusServer } from './services/modbus-server.js';
import { createRuleEngine, RuleEngine } from './services/rule-engine.js';
import { createStateStore, StateStore } from './services/state-store.js';
import { createCompositeRuleEngine, CompositeRuleEngine } from './services/composite-rule-engine.js';
import { createActionRunner, ActionRunner } from './services/action-runner.js';
import { createSignalBridgeService, SignalBridgeService } from './services/signal-bridge.js';
import { createGo2RTCService, Go2RTCService } from './services/go2rtc-service.js';
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
let ruleEngine: RuleEngine | null = null;
let stateStore: StateStore | null = null;
let compositeRuleEngine: CompositeRuleEngine | null = null;
let actionRunner: ActionRunner | null = null;
let signalBridge: SignalBridgeService | null = null;
let go2rtcService: Go2RTCService | null = null;
let pageReloadTimer: NodeJS.Timeout | null = null;
let config: CiraConfig;
let configPath: string | undefined;
let isShuttingDown = false;

// Page reload interval for browser memory stability (25 minutes)
const PAGE_RELOAD_INTERVAL_MS = 25 * 60 * 1000;
const PAGE_RELOAD_WARNING_SECONDS = 30;

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
    // Stop page reload scheduler
    if (pageReloadTimer) {
      clearInterval(pageReloadTimer);
      pageReloadTimer = null;
    }

    // Stop go2rtc service
    if (go2rtcService) {
      logger.debug('Stopping go2rtc service...');
      await go2rtcService.stop();
    }

    // Stop stats collector
    if (statsCollector) {
      logger.debug('Stopping stats collector...');
      statsCollector.stop();
    }

    // Close state store
    if (stateStore) {
      logger.debug('Closing state store...');
      stateStore.close();
    }

    // Stop signal bridge
    if (signalBridge) {
      logger.debug('Stopping signal bridge...');
      await signalBridge.stop();
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
 * Start scheduled page reload for browser memory stability
 * Sends warning 30 seconds before reload, then triggers reload
 */
function startPageReloadScheduler(): void {
  if (pageReloadTimer) {
    clearInterval(pageReloadTimer);
  }

  logger.info(`Page reload scheduler started (interval: ${PAGE_RELOAD_INTERVAL_MS / 60000} minutes)`);

  // Schedule reload cycle
  const scheduleReloadCycle = () => {
    // First, wait for the main interval minus warning time
    setTimeout(() => {
      // Send warning
      if (wsHandler) {
        wsHandler.sendReloadWarning(PAGE_RELOAD_WARNING_SECONDS);
        logger.info(`Sent page reload warning to ${wsHandler.getClientCount()} clients`);
      }

      // Then wait for warning period and trigger reload
      setTimeout(() => {
        if (wsHandler) {
          wsHandler.triggerPageReload('scheduled_memory_cleanup');
        }
        // Schedule next cycle
        scheduleReloadCycle();
      }, PAGE_RELOAD_WARNING_SECONDS * 1000);

    }, PAGE_RELOAD_INTERVAL_MS - (PAGE_RELOAD_WARNING_SECONDS * 1000));
  };

  scheduleReloadCycle();
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

    // Initialize rule engine
    const configDir = path.join(os.homedir(), '.cira');
    const rulesDir = path.join(configDir, 'rules');
    ruleEngine = createRuleEngine(rulesDir);
    setRuleEngine(ruleEngine); // Wire rule engine to API routes
    setStatsCollector(statsCollector); // Wire stats collector for /api/rules/results
    logger.info(`Rule engine initialized: ${rulesDir}`);

    // Spec G: Initialize state store for composite rules
    const stateDbPath = path.join(configDir, 'state.db');
    stateStore = createStateStore(stateDbPath);
    logger.info(`State store initialized: ${stateDbPath}`);

    // Spec G: Initialize composite rule engine
    compositeRuleEngine = createCompositeRuleEngine(stateStore);
    logger.info('Composite rule engine initialized');

    // Spec G: Initialize action runner with available channels
    actionRunner = createActionRunner(mqttChannel, modbusServer);
    logger.info('Action runner initialized');

    // TEMPORARY — Spec D replaces this
    // Inject rule engine into StatsCollector for poll-cycle evaluation.
    // When Spec D (Heartbeat Scheduler) is implemented, remove this line
    // and wire ruleEngine into the HeartbeatScheduler instead.
    if (statsCollector) {
      statsCollector.setRuleEngine(ruleEngine);
      statsCollector.setCompositeRuleEngine(compositeRuleEngine);
      statsCollector.setActionRunner(actionRunner);
      statsCollector.setWebSocketHandler(wsHandler);  // Enable real-time rule execution streaming
    }

    // Register WebChat routes for agent chat (after statsCollector for real data)
    await registerChatRoutes(gateway.fastify, {
      agent,
      nodeManager,
      statsCollector,
      alertsConfig: config.alerts,
      ruleEngine,
      compositeRuleEngine,
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

    // Spec CX-1: Initialize signal protocol bridges
    const signalBridgeConfigDir = path.join(os.homedir(), '.cira');
    signalBridge = createSignalBridgeService(signalBridgeConfigDir, nodeManager, mqttChannel);
    setSignalBridge(signalBridge);
    try {
      await signalBridge.start();
    } catch (error) {
      logger.warn(`Signal bridge failed to start: ${error}`);
      signalBridge = null;
    }

    // Start health checks BEFORE go2rtc so nodes are online when streams are registered
    nodeManager.startHealthChecks(30000); // Every 30 seconds

    // Wait for initial health check to complete (so nodes are online)
    logger.info('Running initial health check for nodes...');
    await new Promise(resolve => setTimeout(resolve, 2000)); // Give health check time to complete

    // Initialize go2rtc service for WebRTC camera streaming (if enabled)
    if (config.gateway.go2rtc?.enabled) {
      go2rtcService = createGo2RTCService({
        enabled: true,
        apiPort: config.gateway.go2rtc.apiPort ?? 1984,
        webrtcPort: config.gateway.go2rtc.webrtcPort ?? 8555,
        binaryPath: config.gateway.go2rtc.binaryPath,
        stunServers: config.gateway.go2rtc.stunServers ?? ['stun:stun.l.google.com:19302'],
      });

      try {
        await go2rtcService.start();
        nodeManager.setGo2RTCService(go2rtcService);
        logger.info(`go2rtc started - API: http://localhost:${config.gateway.go2rtc.apiPort ?? 1984}`);
      } catch (error) {
        logger.warn(`Failed to start go2rtc: ${error}`);
        go2rtcService = null;
      }
    }

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
    if (go2rtcService) {
      logger.info(`  go2rtc API: http://localhost:${config.gateway.go2rtc?.apiPort ?? 1984}`);
      logger.info(`  WebRTC: ws://localhost:${config.gateway.go2rtc?.apiPort ?? 1984}/api/ws`);
    }

    // Log ready status for systemd
    if (process.env.NODE_ENV === 'production') {
      logger.info('Gateway ready and accepting connections');
    }

    // Start scheduled page reload for browser memory stability
    startPageReloadScheduler();

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
