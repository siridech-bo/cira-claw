/// <reference types="vite/client" />

// Declare module for gateway service imports via Vite alias
declare module '@gateway/socket-registry' {
  export type SocketType =
    | 'vision.confidence'
    | 'vision.detection'
    | 'signal.rate'
    | 'signal.threshold'
    | 'system.health'
    | 'any.boolean';

  export const SOCKET_TYPES: readonly SocketType[];

  export const SOCKET_TYPE_LABELS: Record<SocketType, string>;

  export const SOCKET_TYPE_COLORS: Record<SocketType, string>;

  export const PAYLOAD_FIELD_MAP: Record<string, SocketType>;

  export function isValidSocketType(value: string): value is SocketType;

  export function inferSocketType(fields: string[]): SocketType;
}
