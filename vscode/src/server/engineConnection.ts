/**
 * Persistent connection from the language server to the engine's tooling
 * server. Reconnects automatically every 5 seconds while the engine is not
 * running, and re-requests the type database on every (re)connect.
 */

import {
  DiagnosticsMessage,
  LineJsonSocket,
  RawMessage,
  TypeDbClassMessage,
  makeHello,
} from "../common/protocol";
import { TypeDatabase } from "./typedb";

const RECONNECT_INTERVAL_MS = 5000;

export class EngineConnection {
  private socket: LineJsonSocket | undefined;
  private reconnectTimer: NodeJS.Timeout | undefined;
  private stopped = true;
  private host = "127.0.0.1";
  private port = 27500;
  private connectedFlag = false;
  /** True while a bulk type_db_begin .. type_db_end stream is in flight. */
  private receivingBulkTypeDb = false;

  engineInfo: { godotVersion?: string; extensionVersion?: string } = {};

  onDiagnostics: ((message: DiagnosticsMessage) => void) | undefined;
  onTypeDbUpdated: (() => void) | undefined;
  onStatusChange: ((connected: boolean) => void) | undefined;
  onLog: ((message: string) => void) | undefined;

  constructor(
    private readonly db: TypeDatabase,
    private readonly clientName: string = "vscode-lsp"
  ) {}

  get connected(): boolean {
    return this.connectedFlag;
  }

  start(host: string, port: number): void {
    this.stopped = false;
    this.host = host;
    this.port = port;
    this.connect();
  }

  /** Reconnects if the target host/port changed. */
  updateTarget(host: string, port: number): void {
    if (this.host === host && this.port === port && !this.stopped) {
      return;
    }
    this.host = host;
    this.port = port;
    if (!this.stopped) {
      this.log(`Engine target changed to ${host}:${port}; reconnecting`);
      this.disconnectSocket();
      this.connect();
    }
  }

  stop(): void {
    this.stopped = true;
    if (this.reconnectTimer !== undefined) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
    this.disconnectSocket();
  }

  private disconnectSocket(): void {
    if (this.socket !== undefined) {
      this.socket.close();
      this.socket = undefined;
    }
    if (this.connectedFlag) {
      this.connectedFlag = false;
      this.onStatusChange?.(false);
    }
  }

  private connect(): void {
    if (this.stopped) {
      return;
    }
    if (this.reconnectTimer !== undefined) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
    const socket = new LineJsonSocket();
    this.socket = socket;
    socket.onConnect = () => {
      this.connectedFlag = true;
      this.log(`Connected to engine at ${this.host}:${this.port}`);
      this.onStatusChange?.(true);
      socket.send(makeHello(this.clientName));
      socket.send({ type: "request_type_db" });
    };
    socket.onMessage = (message) => this.handleMessage(message);
    socket.onError = () => {
      // The close event follows; reconnection is scheduled there.
    };
    socket.onClose = () => {
      const wasConnected = this.connectedFlag;
      this.connectedFlag = false;
      this.receivingBulkTypeDb = false;
      if (wasConnected) {
        this.log("Engine connection closed");
        this.onStatusChange?.(false);
      }
      this.scheduleReconnect();
    };
    socket.connect(this.host, this.port);
  }

  private scheduleReconnect(): void {
    if (this.stopped || this.reconnectTimer !== undefined) {
      return;
    }
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      this.connect();
    }, RECONNECT_INTERVAL_MS);
  }

  private handleMessage(message: RawMessage): void {
    switch (message.type) {
      case "welcome": {
        this.engineInfo = {
          godotVersion: typeof message.godot_version === "string" ? message.godot_version : undefined,
          extensionVersion:
            typeof message.extension_version === "string" ? message.extension_version : undefined,
        };
        this.log(
          `Engine welcome: godot ${this.engineInfo.godotVersion ?? "?"}, extension ${this.engineInfo.extensionVersion ?? "?"}`
        );
        break;
      }
      case "type_db_begin": {
        this.receivingBulkTypeDb = true;
        this.db.clear();
        break;
      }
      case "type_db_class": {
        this.db.ingestClass(message as unknown as TypeDbClassMessage);
        if (!this.receivingBulkTypeDb) {
          // Script classes are re-sent individually after module rebuilds.
          this.onTypeDbUpdated?.();
        }
        break;
      }
      case "type_db_end": {
        this.receivingBulkTypeDb = false;
        this.log(`Type database loaded: ${this.db.count} classes`);
        this.onTypeDbUpdated?.();
        break;
      }
      case "diagnostics": {
        this.onDiagnostics?.(message as unknown as DiagnosticsMessage);
        break;
      }
      default:
        // Unknown message types (and debugger traffic) are ignored.
        break;
    }
  }

  private log(message: string): void {
    this.onLog?.(message);
  }
}
