import pino from 'pino';

const isDev = process.env.NODE_ENV !== 'production';

// In production, log to stdout (fd 1) so systemd/journald captures all logs
// In development, use pino-pretty for human-readable output
export const logger = pino(
  {
    level: process.env.LOG_LEVEL || (isDev ? 'debug' : 'info'),
    // Add timestamp in production for journald
    timestamp: !isDev ? pino.stdTimeFunctions.isoTime : false,
    // Format for journald compatibility
    formatters: {
      level: (label) => ({ level: label }),
    },
  },
  isDev
    ? pino.transport({
        target: 'pino-pretty',
        options: {
          colorize: true,
          translateTime: 'SYS:standard',
          ignore: 'pid,hostname',
        },
      })
    : pino.destination(1) // stdout for systemd/journald
);

export const createLogger = (name: string) => {
  return logger.child({ name });
};

export default logger;
