import fs from 'fs';
import path from 'path';
import os from 'os';
import { CiraConfigSchema, CiraConfig, NodeConfigSchema, NodeConfig } from './utils/config-schema.js';
import { createLogger } from './utils/logger.js';

const logger = createLogger('config');

// Resolve ~ to home directory
function resolvePath(p: string): string {
  if (p.startsWith('~')) {
    return path.join(os.homedir(), p.slice(1));
  }
  return p;
}

// Expand environment variables in string values
function expandEnvVars(value: string): string {
  return value.replace(/\$\{([^}]+)\}/g, (_, name) => {
    return process.env[name] || '';
  });
}

// Recursively expand env vars in object
function expandEnvVarsInObject(obj: unknown): unknown {
  if (typeof obj === 'string') {
    return expandEnvVars(obj);
  }
  if (Array.isArray(obj)) {
    return obj.map(expandEnvVarsInObject);
  }
  if (obj !== null && typeof obj === 'object') {
    const result: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(obj)) {
      result[key] = expandEnvVarsInObject(value);
    }
    return result;
  }
  return obj;
}

export class ConfigLoader {
  private ciraHome: string;
  private config: CiraConfig | null = null;
  private nodes: Map<string, NodeConfig> = new Map();

  constructor(ciraHome?: string) {
    this.ciraHome = resolvePath(ciraHome || process.env.CIRA_HOME || '~/.cira');
  }

  get home(): string {
    return this.ciraHome;
  }

  get configPath(): string {
    return path.join(this.ciraHome, 'cira.json');
  }

  get workspacePath(): string {
    if (this.config?.agent.workspace) {
      return resolvePath(this.config.agent.workspace);
    }
    return path.join(this.ciraHome, 'workspace');
  }

  get nodesPath(): string {
    return path.join(this.ciraHome, 'nodes');
  }

  get credentialsPath(): string {
    return path.join(this.ciraHome, 'credentials');
  }

  get logsPath(): string {
    return path.join(this.ciraHome, 'logs');
  }

  // Initialize directory structure
  async init(): Promise<void> {
    const dirs = [
      this.ciraHome,
      this.workspacePath,
      path.join(this.workspacePath, 'skills'),
      path.join(this.workspacePath, 'models'),
      this.nodesPath,
      this.credentialsPath,
      this.logsPath,
    ];

    for (const dir of dirs) {
      if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
        logger.debug(`Created directory: ${dir}`);
      }
    }

    // Create default config if it doesn't exist
    if (!fs.existsSync(this.configPath)) {
      const defaultConfig = CiraConfigSchema.parse({});
      fs.writeFileSync(this.configPath, JSON.stringify(defaultConfig, null, 2));
      logger.info(`Created default config at ${this.configPath}`);
    }
  }

  // Load main configuration
  async load(configPath?: string): Promise<CiraConfig> {
    const cfgPath = configPath || this.configPath;

    if (!fs.existsSync(cfgPath)) {
      throw new Error(`Configuration file not found: ${cfgPath}`);
    }

    try {
      const rawContent = fs.readFileSync(cfgPath, 'utf-8');
      const rawConfig = JSON.parse(rawContent);
      const expandedConfig = expandEnvVarsInObject(rawConfig);

      const result = CiraConfigSchema.safeParse(expandedConfig);

      if (!result.success) {
        const errors = result.error.errors.map(e => `  - ${e.path.join('.')}: ${e.message}`).join('\n');
        throw new Error(`Invalid configuration:\n${errors}`);
      }

      this.config = result.data;
      logger.info(`Loaded configuration from ${cfgPath}`);
      return this.config;
    } catch (error) {
      if (error instanceof SyntaxError) {
        throw new Error(`Invalid JSON in configuration file: ${cfgPath}`);
      }
      throw error;
    }
  }

  // Get current configuration
  getConfig(): CiraConfig {
    if (!this.config) {
      throw new Error('Configuration not loaded. Call load() first.');
    }
    return this.config;
  }

  // Load all node configurations
  async loadNodes(): Promise<Map<string, NodeConfig>> {
    this.nodes.clear();

    if (!fs.existsSync(this.nodesPath)) {
      logger.debug('Nodes directory does not exist');
      return this.nodes;
    }

    const files = fs.readdirSync(this.nodesPath).filter(f => f.endsWith('.json'));

    for (const file of files) {
      try {
        const filePath = path.join(this.nodesPath, file);
        const rawContent = fs.readFileSync(filePath, 'utf-8');
        const rawConfig = JSON.parse(rawContent);
        const expandedConfig = expandEnvVarsInObject(rawConfig);

        const result = NodeConfigSchema.safeParse(expandedConfig);

        if (!result.success) {
          logger.warn(`Invalid node config in ${file}: ${result.error.message}`);
          continue;
        }

        this.nodes.set(result.data.id, result.data);
        logger.debug(`Loaded node: ${result.data.id}`);
      } catch (error) {
        logger.warn(`Failed to load node config ${file}: ${error}`);
      }
    }

    logger.info(`Loaded ${this.nodes.size} node configurations`);
    return this.nodes;
  }

  // Get a specific node
  getNode(id: string): NodeConfig | undefined {
    return this.nodes.get(id);
  }

  // Get all nodes
  getAllNodes(): NodeConfig[] {
    return Array.from(this.nodes.values());
  }

  // Save a node configuration
  async saveNode(node: NodeConfig): Promise<void> {
    const result = NodeConfigSchema.safeParse(node);
    if (!result.success) {
      throw new Error(`Invalid node configuration: ${result.error.message}`);
    }

    const filePath = path.join(this.nodesPath, `${node.id}.json`);
    fs.writeFileSync(filePath, JSON.stringify(result.data, null, 2));
    this.nodes.set(node.id, result.data);
    logger.info(`Saved node configuration: ${node.id}`);
  }

  // Delete a node configuration
  async deleteNode(id: string): Promise<boolean> {
    const filePath = path.join(this.nodesPath, `${id}.json`);
    if (fs.existsSync(filePath)) {
      fs.unlinkSync(filePath);
      this.nodes.delete(id);
      logger.info(`Deleted node configuration: ${id}`);
      return true;
    }
    return false;
  }

  // Save main configuration
  async save(): Promise<void> {
    if (!this.config) {
      throw new Error('No configuration to save');
    }
    fs.writeFileSync(this.configPath, JSON.stringify(this.config, null, 2));
    logger.info(`Saved configuration to ${this.configPath}`);
  }
}

// Singleton instance
let configLoaderInstance: ConfigLoader | null = null;

export function getConfigLoader(ciraHome?: string): ConfigLoader {
  if (!configLoaderInstance) {
    configLoaderInstance = new ConfigLoader(ciraHome);
  }
  return configLoaderInstance;
}

export default ConfigLoader;
