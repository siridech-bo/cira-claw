#!/usr/bin/env node

import { Command } from 'commander';
import { execSync, spawn } from 'child_process';
import { getConfigLoader } from '../src/config.js';
import { getNodeManager } from '../src/nodes/manager.js';
import { getAgent } from '../src/agent/agent.js';
import { createLogger } from '../src/utils/logger.js';
import readline from 'readline';
import path from 'path';
import fs from 'fs';

const logger = createLogger('cli');
const program = new Command();

const SERVICE_NAME = 'cira-claw';

program
  .name('cira')
  .description('CiRA Edge Agent CLI')
  .version('1.0.0');

// Node commands
const nodeCmd = program.command('node').description('Device management commands');

nodeCmd
  .command('list')
  .description('List all registered nodes')
  .action(async () => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      await configLoader.load();

      const nodeManager = getNodeManager(configLoader);
      await nodeManager.init();

      const nodes = nodeManager.getAllNodes();
      const statuses = nodeManager.getAllStatuses();

      if (nodes.length === 0) {
        console.log('No nodes registered.');
        return;
      }

      console.log('\nRegistered Nodes:\n');
      console.log('ID                 Name                      Type           Host              Status');
      console.log('-'.repeat(90));

      for (const node of nodes) {
        const status = statuses.find(s => s.id === node.id);
        const statusStr = status?.status || 'unknown';
        const statusIcon = statusStr === 'online' ? '🟢' : statusStr === 'offline' ? '🔴' : '⚪';

        console.log(
          `${node.id.padEnd(18)} ${node.name.padEnd(25)} ${node.type.padEnd(14)} ${node.host.padEnd(17)} ${statusIcon} ${statusStr}`
        );
      }

      const summary = nodeManager.getSummary();
      console.log(`\nTotal: ${summary.total} | Online: ${summary.online} | Offline: ${summary.offline}`);
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

nodeCmd
  .command('status <id>')
  .description('Get status of a specific node')
  .action(async (id: string) => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      await configLoader.load();

      const nodeManager = getNodeManager(configLoader);
      await nodeManager.init();

      const node = nodeManager.getNode(id);
      if (!node) {
        console.error(`Node '${id}' not found.`);
        process.exit(1);
      }

      console.log(`\nChecking status of ${node.name} (${id})...`);

      const status = await nodeManager.checkNodeHealth(node);

      console.log(`\nNode: ${node.name}`);
      console.log(`ID: ${node.id}`);
      console.log(`Type: ${node.type}`);
      console.log(`Host: ${node.host}`);
      console.log(`Status: ${status.status}`);

      if (status.metrics) {
        console.log(`\nMetrics:`);
        console.log(`  FPS: ${status.metrics.fps ?? 'N/A'}`);
        console.log(`  Temperature: ${status.metrics.temperature ? `${status.metrics.temperature}°C` : 'N/A'}`);
        console.log(`  CPU: ${status.metrics.cpuUsage ? `${status.metrics.cpuUsage}%` : 'N/A'}`);
        console.log(`  Memory: ${status.metrics.memoryUsage ? `${status.metrics.memoryUsage}%` : 'N/A'}`);
      }

      if (status.inference) {
        console.log(`\nInference:`);
        console.log(`  Model: ${status.inference.modelName || 'N/A'}`);
        console.log(`  Defects Total: ${status.inference.defectsTotal}`);
        console.log(`  Defects/Hour: ${status.inference.defectsPerHour}`);
      }
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

nodeCmd
  .command('add <host>')
  .description('Add a new node')
  .option('-n, --name <name>', 'Node name')
  .option('-t, --type <type>', 'Node type (jetson-nano, jetson-nx, raspberry-pi, generic)', 'generic')
  .option('-i, --id <id>', 'Node ID (defaults to hostname)')
  .action(async (host: string, options: { name?: string; type?: string; id?: string }) => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      await configLoader.load();

      const nodeManager = getNodeManager(configLoader);
      await nodeManager.init();

      const id = options.id || host.replace(/\./g, '-');
      const name = options.name || `Node ${host}`;
      const type = (options.type || 'generic') as 'jetson-nano' | 'jetson-nx' | 'jetson-agx' | 'raspberry-pi' | 'generic';

      await nodeManager.addNode({
        id,
        name,
        type,
        host,
        ssh: { user: 'cira', port: 22 },
        runtime: { port: 8080, config: '/home/cira/.cira/model_config.json' },
        cameras: [],
        models: [],
      });

      console.log(`\nNode added successfully!`);
      console.log(`  ID: ${id}`);
      console.log(`  Name: ${name}`);
      console.log(`  Host: ${host}`);
      console.log(`  Type: ${type}`);
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

// Model commands
const modelCmd = program.command('model').description('Model management commands');

modelCmd
  .command('list')
  .description('List available models')
  .action(async () => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      await configLoader.load();

      const modelsPath = configLoader.workspacePath + '/models';
      console.log(`\nModels directory: ${modelsPath}`);
      console.log('\nNote: Model registry not yet implemented. Check the models directory manually.');
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

// Agent commands
const agentCmd = program.command('agent').description('AI agent commands');

agentCmd
  .command('query <prompt>')
  .description('Send a single query to the AI agent')
  .action(async (prompt: string) => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      const config = await configLoader.load();

      const nodeManager = getNodeManager(configLoader);
      await nodeManager.init();

      const agent = getAgent(config.agent, configLoader.workspacePath);
      await agent.init();

      if (!agent.isAvailable()) {
        console.error('AI agent is not available. Please configure your Anthropic API key.');
        console.log('Set ANTHROPIC_API_KEY environment variable or create ~/.cira/credentials/claude.json');
        process.exit(1);
      }

      console.log('Thinking...\n');

      const response = await agent.query(prompt, { nodeManager: nodeManager as unknown as undefined });
      console.log(response);
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

agentCmd
  .command('interactive')
  .alias('chat')
  .description('Start interactive chat with the AI agent')
  .action(async () => {
    try {
      const configLoader = getConfigLoader();
      await configLoader.init();
      const config = await configLoader.load();

      const nodeManager = getNodeManager(configLoader);
      await nodeManager.init();

      const agent = getAgent(config.agent, configLoader.workspacePath);
      await agent.init();

      if (!agent.isAvailable()) {
        console.error('AI agent is not available. Please configure your Anthropic API key.');
        console.log('Set ANTHROPIC_API_KEY environment variable or create ~/.cira/credentials/claude.json');
        process.exit(1);
      }

      console.log('\nCiRA CLAW - Interactive Mode');
      console.log('Type "exit" or press Ctrl+C to quit.\n');

      const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout,
      });

      const promptUser = () => {
        rl.question('You: ', async (input) => {
          const trimmed = input.trim();

          if (trimmed.toLowerCase() === 'exit' || trimmed.toLowerCase() === 'quit') {
            console.log('\nGoodbye!');
            rl.close();
            process.exit(0);
          }

          if (!trimmed) {
            promptUser();
            return;
          }

          console.log('\nCiRA: Thinking...');

          try {
            const response = await agent.query(trimmed, { nodeManager: nodeManager as unknown as undefined });
            console.log(`\nCiRA: ${response}\n`);
          } catch (error) {
            console.log(`\nCiRA: Error - ${error instanceof Error ? error.message : 'Unknown error'}\n`);
          }

          promptUser();
        });
      };

      promptUser();
    } catch (error) {
      console.error('Error:', error instanceof Error ? error.message : error);
      process.exit(1);
    }
  });

// Service commands
const serviceCmd = program.command('service').description('Daemon service management (systemd)');

/**
 * Check if running on Linux with systemd
 */
function checkSystemd(): boolean {
  if (process.platform !== 'linux') {
    console.error('Error: Service management requires Linux with systemd');
    return false;
  }
  try {
    execSync('systemctl --version', { stdio: 'ignore' });
    return true;
  } catch {
    console.error('Error: systemd not found');
    return false;
  }
}

/**
 * Run systemctl command
 */
function runSystemctl(args: string[], sudo: boolean = false): void {
  const cmd = sudo ? ['sudo', 'systemctl', ...args] : ['systemctl', ...args];
  const proc = spawn(cmd[0], cmd.slice(1), { stdio: 'inherit' });

  proc.on('close', (code) => {
    process.exit(code || 0);
  });

  proc.on('error', (err) => {
    console.error(`Failed to run systemctl: ${err.message}`);
    process.exit(1);
  });
}

serviceCmd
  .command('install')
  .description('Install the gateway as a systemd service')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    const scriptPath = path.join(process.cwd(), 'scripts', 'install.sh');
    if (!fs.existsSync(scriptPath)) {
      console.error(`Error: Install script not found at ${scriptPath}`);
      console.log('Make sure you are in the cira-edge project directory.');
      process.exit(1);
    }

    console.log('Installing CiRA Edge Gateway service...');
    console.log('This requires root privileges.\n');

    const proc = spawn('sudo', ['bash', scriptPath], { stdio: 'inherit' });

    proc.on('close', (code) => {
      process.exit(code || 0);
    });

    proc.on('error', (err) => {
      console.error(`Failed to run install script: ${err.message}`);
      process.exit(1);
    });
  });

serviceCmd
  .command('start')
  .description('Start the gateway daemon')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Starting ${SERVICE_NAME}...`);
    runSystemctl(['start', SERVICE_NAME], true);
  });

serviceCmd
  .command('stop')
  .description('Stop the gateway daemon')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Stopping ${SERVICE_NAME}...`);
    runSystemctl(['stop', SERVICE_NAME], true);
  });

serviceCmd
  .command('restart')
  .description('Restart the gateway daemon')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Restarting ${SERVICE_NAME}...`);
    runSystemctl(['restart', SERVICE_NAME], true);
  });

serviceCmd
  .command('reload')
  .description('Reload configuration (sends SIGHUP)')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Reloading ${SERVICE_NAME} configuration...`);
    runSystemctl(['reload', SERVICE_NAME], true);
  });

serviceCmd
  .command('status')
  .description('Check daemon status')
  .action(async () => {
    // First try to check if the service is running via HTTP
    try {
      const response = await fetch('http://localhost:18790/api/status', {
        signal: AbortSignal.timeout(3000),
      });
      if (response.ok) {
        const data = await response.json() as { name: string; version: string; uptime: number };
        console.log('\n🟢 CiRA CLAW is running\n');
        console.log(`  Name: ${data.name}`);
        console.log(`  Version: ${data.version}`);
        console.log(`  Uptime: ${Math.floor(data.uptime / 60)} minutes`);
        console.log(`  Dashboard: http://localhost:18790\n`);
      } else {
        console.log('\n🟡 Gateway responding but with error');
      }
    } catch {
      console.log('\n🔴 Gateway is not responding on http://localhost:18790\n');
    }

    // Also show systemd status if available
    if (process.platform === 'linux') {
      try {
        execSync('systemctl --version', { stdio: 'ignore' });
        console.log('Systemd service status:');
        runSystemctl(['status', SERVICE_NAME, '--no-pager']);
      } catch {
        // systemd not available, skip
      }
    }
  });

serviceCmd
  .command('logs')
  .description('View gateway logs (journalctl)')
  .option('-f, --follow', 'Follow log output')
  .option('-n, --lines <n>', 'Number of lines to show', '50')
  .action((options: { follow?: boolean; lines?: string }) => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    const args = ['-u', SERVICE_NAME, '--no-pager'];

    if (options.follow) {
      args.push('-f');
    } else {
      args.push('-n', options.lines || '50');
    }

    const proc = spawn('journalctl', args, { stdio: 'inherit' });

    proc.on('close', (code) => {
      process.exit(code || 0);
    });

    proc.on('error', (err) => {
      console.error(`Failed to run journalctl: ${err.message}`);
      process.exit(1);
    });
  });

serviceCmd
  .command('enable')
  .description('Enable service to start on boot')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Enabling ${SERVICE_NAME} to start on boot...`);
    runSystemctl(['enable', SERVICE_NAME], true);
  });

serviceCmd
  .command('disable')
  .description('Disable service from starting on boot')
  .action(() => {
    if (!checkSystemd()) {
      process.exit(1);
    }

    console.log(`Disabling ${SERVICE_NAME} from starting on boot...`);
    runSystemctl(['disable', SERVICE_NAME], true);
  });

// Onboard command
program
  .command('onboard')
  .description('Interactive setup wizard')
  .action(async () => {
    console.log('\n=== CiRA Edge Gateway Setup ===\n');

    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout,
    });

    const question = (prompt: string): Promise<string> => {
      return new Promise(resolve => {
        rl.question(prompt, resolve);
      });
    };

    try {
      const configLoader = getConfigLoader();
      await configLoader.init();

      console.log(`Configuration directory: ${configLoader.home}`);
      console.log(`Workspace: ${configLoader.workspacePath}\n`);

      const gatewayName = await question('Gateway name [CiRA Edge Gateway]: ');
      const apiKey = await question('Anthropic API key (optional, press Enter to skip): ');

      // Load and update config
      const config = await configLoader.load();

      if (gatewayName) {
        config.gateway.name = gatewayName;
      }

      await configLoader.save();
      console.log(`\nConfiguration saved to ${configLoader.configPath}`);

      // Save API key if provided
      if (apiKey) {
        const credPath = configLoader.credentialsPath + '/claude.json';
        fs.writeFileSync(credPath, JSON.stringify({ api_key: apiKey }, null, 2));
        console.log(`API key saved to ${credPath}`);
      }

      console.log('\n=== Setup Complete ===');
      console.log('\nNext steps:');
      console.log('1. Start the gateway: npm run dev');
      console.log('2. Open dashboard: http://localhost:18790');
      console.log('3. Add nodes: cira node add <ip-address>');
      console.log('4. Chat with agent: cira agent interactive\n');

      rl.close();
    } catch (error) {
      console.error('Setup failed:', error instanceof Error ? error.message : error);
      rl.close();
      process.exit(1);
    }
  });

program.parse();
