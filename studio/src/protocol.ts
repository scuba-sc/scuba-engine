// Wire shapes of the scuba-hostd bridge (docs/protocol.md).

export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

export type RequestId = string | number;

export interface BridgeRequest {
  id: RequestId;
  method: string;
  params?: JsonValue;
}

export interface BridgeErrorBody {
  code: number;
  message: string;
  data?: JsonValue;
}

export interface BridgeResponse {
  id: RequestId | null;
  result?: JsonValue;
  error?: BridgeErrorBody;
}

export interface BridgeEvent {
  event: string;
  data: JsonValue;
}

export const RpcErrorCode = {
  ParseError: -32700,
  InvalidRequest: -32600,
  MethodNotFound: -32601,
  InvalidParams: -32602,
  InternalError: -32603,
  HandlerFailed: -32000,
  Unavailable: -32001,
} as const;

export interface KernelPluginStatus {
  folder: string;
  id: string;
  name: string;
  version: string;
  state: string;
  reason: string;
}

export interface SystemStatus {
  name: string;
  version: string;
  sdk_version: string;
  uptime_ms: number;
  clients: number;
  kernel: {
    started: boolean;
    plugin_root: string;
    plugins: KernelPluginStatus[];
  };
}

export interface PingResult {
  pong: true;
  uptime_ms: number;
  echo?: JsonValue;
}
