import { z } from 'zod';

// Gateway configuration schema
export const GatewayConfigSchema = z.object({
  port: z.number().int().min(1).max(65535).default(18790),
  host: z.string().default('0.0.0.0'),
  name: z.string().default('CiRA Edge Gateway'),
});

// Agent configuration schema
export const AgentConfigSchema = z.object({
  provider: z.enum(['claude', 'ollama']).default('claude'),
  model: z.string().default('claude-sonnet-4-20250514'),
  fallback: z.enum(['ollama', 'none']).default('ollama'),
  ollama_url: z.string().url().default('http://localhost:11434'),
  ollama_model: z.string().default('llama3.1:8b'),
  workspace: z.string().default('~/.cira/workspace'),
});

// LINE channel configuration
export const LineChannelConfigSchema = z.object({
  enabled: z.boolean().default(false),
  channel_access_token: z.string().optional(),
  channel_secret: z.string().optional(),
});

// Telegram channel configuration
export const TelegramChannelConfigSchema = z.object({
  enabled: z.boolean().default(false),
  bot_token: z.string().optional(),
});

// MQTT channel configuration
export const MqttChannelConfigSchema = z.object({
  enabled: z.boolean().default(true),
  broker: z.string().default('mqtt://localhost:1883'),
  topics: z.object({
    subscribe: z.array(z.string()).default(['cira/command/#']),
    publish: z.array(z.string()).default(['cira/results/#', 'cira/alerts/#']),
  }).default({}),
});

// WebChat channel configuration
export const WebChatChannelConfigSchema = z.object({
  enabled: z.boolean().default(true),
  path: z.string().default('/chat'),
});

// Channels configuration
export const ChannelsConfigSchema = z.object({
  line: LineChannelConfigSchema.default({}),
  telegram: TelegramChannelConfigSchema.default({}),
  mqtt: MqttChannelConfigSchema.default({}),
  webchat: WebChatChannelConfigSchema.default({}),
});

// Discovery configuration
export const DiscoveryConfigSchema = z.object({
  mdns: z.object({
    enabled: z.boolean().default(true),
    service_type: z.string().default('_cira._tcp'),
  }).default({}),
  manual: z.array(z.string()).default([]),
});

// Alerts configuration
export const AlertsConfigSchema = z.object({
  defect_threshold: z.number().int().min(0).default(10),
  temperature_max: z.number().min(0).max(150).default(80),
  fps_min: z.number().min(0).default(5),
  notify_channels: z.array(z.string()).default(['line', 'mqtt']),
});

// Main CiRA configuration schema
export const CiraConfigSchema = z.object({
  gateway: GatewayConfigSchema.default({}),
  agent: AgentConfigSchema.default({}),
  channels: ChannelsConfigSchema.default({}),
  discovery: DiscoveryConfigSchema.default({}),
  alerts: AlertsConfigSchema.default({}),
});

export type CiraConfig = z.infer<typeof CiraConfigSchema>;
export type GatewayConfig = z.infer<typeof GatewayConfigSchema>;
export type AgentConfig = z.infer<typeof AgentConfigSchema>;
export type ChannelsConfig = z.infer<typeof ChannelsConfigSchema>;
export type DiscoveryConfig = z.infer<typeof DiscoveryConfigSchema>;
export type AlertsConfig = z.infer<typeof AlertsConfigSchema>;

// Node configuration schema
export const NodeSshConfigSchema = z.object({
  user: z.string().default('cira'),
  key: z.string().optional(),
  password: z.string().optional(),
  port: z.number().int().default(22),
});

export const NodeRuntimeConfigSchema = z.object({
  port: z.number().int().default(8080),
  config: z.string().default('/home/cira/.cira/model_config.json'),
});

export const NodeCameraConfigSchema = z.object({
  id: z.string(),
  device: z.number().int().default(0),
  name: z.string(),
  resolution: z.string().default('1280x720'),
});

export const NodeModelConfigSchema = z.object({
  name: z.string(),
  task: z.enum(['detection', 'classification', 'anomaly']),
  labels: z.array(z.string()),
});

export const NodeConfigSchema = z.object({
  id: z.string(),
  name: z.string(),
  type: z.enum(['jetson-nano', 'jetson-nx', 'jetson-agx', 'raspberry-pi', 'generic']),
  host: z.string(),
  ssh: NodeSshConfigSchema.default({}),
  runtime: NodeRuntimeConfigSchema.default({}),
  cameras: z.array(NodeCameraConfigSchema).default([]),
  models: z.array(NodeModelConfigSchema).default([]),
  location: z.string().optional(),
});

export type NodeConfig = z.infer<typeof NodeConfigSchema>;

// Model configuration schema (for CiRA Runtime)
export const ModelSourceConfigSchema = z.object({
  type: z.enum(['camera', 'file', 'rtsp', 'http']),
  device: z.number().int().optional(),
  path: z.string().optional(),
  url: z.string().optional(),
  width: z.number().int().default(1280),
  height: z.number().int().default(720),
  fps: z.number().int().default(30),
});

export const ModelStreamConfigSchema = z.object({
  enabled: z.boolean().default(true),
  port: z.number().int().default(8080),
  mjpeg: z.object({
    enabled: z.boolean().default(true),
    quality: z.number().int().min(1).max(100).default(80),
    max_fps: z.number().int().default(15),
    endpoints: z.object({
      raw: z.string().default('/stream/raw'),
      annotated: z.string().default('/stream/annotated'),
    }).default({}),
  }).default({}),
  websocket: z.object({
    enabled: z.boolean().default(true),
    path: z.string().default('/ws/video'),
    format: z.enum(['jpeg', 'png', 'raw']).default('jpeg'),
    include_json: z.boolean().default(true),
  }).default({}),
  snapshot: z.object({
    endpoint: z.string().default('/snapshot'),
    include_annotation: z.boolean().default(true),
  }).default({}),
});

export const ModelAnnotationConfigSchema = z.object({
  bbox_color: z.tuple([z.number(), z.number(), z.number()]).default([0, 255, 0]),
  bbox_thickness: z.number().int().default(2),
  label_size: z.number().default(0.6),
  show_confidence: z.boolean().default(true),
  show_fps: z.boolean().default(true),
  show_timestamp: z.boolean().default(true),
});

export const ModelOutputMqttConfigSchema = z.object({
  enabled: z.boolean().default(false),
  broker: z.string().default('mqtt://localhost:1883'),
  topic: z.string().default('cira/results'),
});

export const ModelOutputRestConfigSchema = z.object({
  enabled: z.boolean().default(true),
  port: z.number().int().default(8080),
  endpoint: z.string().default('/api/results'),
});

export const RuntimeModelConfigSchema = z.object({
  name: z.string(),
  version: z.string().default('1.0.0'),
  task: z.enum(['detection', 'classification', 'anomaly']),
  model: z.object({
    path: z.string(),
    format: z.enum(['auto', 'darknet', 'onnx', 'tensorrt', 'sklearn']).default('auto'),
    labels: z.array(z.string()),
    input_size: z.tuple([z.number(), z.number()]).default([416, 416]),
    confidence_threshold: z.number().min(0).max(1).default(0.5),
    nms_threshold: z.number().min(0).max(1).default(0.4),
  }),
  source: ModelSourceConfigSchema,
  stream: ModelStreamConfigSchema.default({}),
  annotation: ModelAnnotationConfigSchema.default({}),
  output: z.object({
    mqtt: ModelOutputMqttConfigSchema.default({}),
    rest: ModelOutputRestConfigSchema.default({}),
  }).default({}),
});

export type RuntimeModelConfig = z.infer<typeof RuntimeModelConfigSchema>;
