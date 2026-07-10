/**
 * Wire protocol shared between the language server and the debug adapter.
 *
 * See docs/PROTOCOL.md in the repository root: one JSON object per line,
 * UTF-8, terminated by "\n". Every message has a "type" field; unknown
 * message types must be ignored.
 */

import * as net from "node:net";
import * as path from "node:path";

export const PROTOCOL_VERSION = 1;
export const DEFAULT_PORT = 27500;
export const DEFAULT_HOST = "127.0.0.1";
export const RES_PREFIX = "res://";

// ---------------------------------------------------------------------------
// Client -> engine messages
// ---------------------------------------------------------------------------

export interface HelloMessage {
  type: "hello";
  client: string;
  version: number;
}

export interface RequestTypeDbMessage {
  type: "request_type_db";
}

export interface SetBreakpointsMessage {
  type: "set_breakpoints";
  path: string;
  lines: number[];
}

export interface ContinueMessage {
  type: "continue";
}

export interface PauseMessage {
  type: "pause";
}

export interface StepOverMessage {
  type: "step_over";
}

export interface StepInMessage {
  type: "step_in";
}

export interface StepOutMessage {
  type: "step_out";
}

export interface RequestStackMessage {
  type: "request_stack";
}

export type VariableScopeName = "locals" | "members";

export interface RequestVariablesMessage {
  type: "request_variables";
  frame: number;
  scope: VariableScopeName;
}

export interface RequestEvaluateMessage {
  type: "request_evaluate";
  frame: number;
  expression: string;
}

export type ClientMessage =
  | HelloMessage
  | RequestTypeDbMessage
  | SetBreakpointsMessage
  | ContinueMessage
  | PauseMessage
  | StepOverMessage
  | StepInMessage
  | StepOutMessage
  | RequestStackMessage
  | RequestVariablesMessage
  | RequestEvaluateMessage;

// ---------------------------------------------------------------------------
// Engine -> client messages
// ---------------------------------------------------------------------------

export interface WelcomeMessage {
  type: "welcome";
  engine: string;
  godot_version: string;
  extension_version: string;
  version: number;
}

export interface TypeDbArg {
  name: string;
  type: string;
  default?: string | null;
}

export interface TypeDbMethod {
  name: string;
  return: string;
  args: TypeDbArg[];
  static?: boolean;
  vararg?: boolean;
}

export interface TypeDbProperty {
  name: string;
  type: string;
}

export interface TypeDbSignal {
  name: string;
  args: TypeDbArg[];
}

export interface TypeDbConstant {
  name: string;
  value: number | string;
}

export interface TypeDbEnumValue {
  name: string;
  value: number;
}

export interface TypeDbEnum {
  name: string;
  values: TypeDbEnumValue[];
}

export interface TypeDbBeginMessage {
  type: "type_db_begin";
}

export interface TypeDbClassMessage {
  type: "type_db_class";
  name: string;
  base?: string;
  native: boolean;
  /** Only present for script classes (native: false). */
  path?: string;
  line?: number;
  methods?: TypeDbMethod[];
  properties?: TypeDbProperty[];
  signals?: TypeDbSignal[];
  constants?: TypeDbConstant[];
  enums?: TypeDbEnum[];
}

export interface TypeDbEndMessage {
  type: "type_db_end";
  count: number;
}

export type DiagnosticSeverityName = "error" | "warning" | "info";

export interface DiagnosticItem {
  line: number;
  column: number;
  severity: DiagnosticSeverityName;
  message: string;
}

export interface DiagnosticsMessage {
  type: "diagnostics";
  path: string;
  items: DiagnosticItem[];
}

export interface BreakpointsSetMessage {
  type: "breakpoints_set";
  path: string;
  verified: number[];
}

export type StoppedReason = "breakpoint" | "step" | "pause" | "exception";

export interface StoppedMessage {
  type: "stopped";
  reason: StoppedReason;
  path: string;
  line: number;
  text: string;
}

export interface ContinuedMessage {
  type: "continued";
}

export interface ExitedMessage {
  type: "exited";
}

export interface StackFrameInfo {
  id: number;
  func: string;
  path: string;
  line: number;
}

export interface StackMessage {
  type: "stack";
  frames: StackFrameInfo[];
}

export interface VariableInfo {
  name: string;
  value: string;
  type: string;
}

export interface VariablesMessage {
  type: "variables";
  frame: number;
  scope: VariableScopeName;
  variables: VariableInfo[];
}

export interface EvaluateMessage {
  type: "evaluate";
  value: string;
}

export type EngineMessage =
  | WelcomeMessage
  | TypeDbBeginMessage
  | TypeDbClassMessage
  | TypeDbEndMessage
  | DiagnosticsMessage
  | BreakpointsSetMessage
  | StoppedMessage
  | ContinuedMessage
  | ExitedMessage
  | StackMessage
  | VariablesMessage
  | EvaluateMessage;

/** Any parsed line: engine messages plus unknown types we must ignore. */
export interface RawMessage {
  type: string;
  [key: string]: unknown;
}

export function makeHello(client: string): HelloMessage {
  return { type: "hello", client, version: PROTOCOL_VERSION };
}

// ---------------------------------------------------------------------------
// Path mapping (res:// <-> disk)
// ---------------------------------------------------------------------------

/** Maps a res:// path to an absolute disk path under the project root. */
export function resPathToDiskPath(resPath: string, projectRoot: string): string | undefined {
  if (!resPath.startsWith(RES_PREFIX) || projectRoot.length === 0) {
    return undefined;
  }
  const relative = resPath.slice(RES_PREFIX.length);
  return path.join(projectRoot, ...relative.split("/"));
}

/** Maps an absolute disk path inside the project root to a res:// path. */
export function diskPathToResPath(diskPath: string, projectRoot: string): string | undefined {
  if (projectRoot.length === 0) {
    return undefined;
  }
  const relative = path.relative(projectRoot, diskPath);
  if (relative.length === 0 || relative.startsWith("..") || path.isAbsolute(relative)) {
    return undefined;
  }
  return RES_PREFIX + relative.split(path.sep).join("/");
}

// ---------------------------------------------------------------------------
// Line-delimited JSON socket
// ---------------------------------------------------------------------------

/**
 * A thin wrapper around a TCP socket that frames messages as one JSON object
 * per "\n"-terminated line, per docs/PROTOCOL.md. Lines that fail to parse
 * are silently dropped (the protocol requires unknown input to be ignored).
 */
export class LineJsonSocket {
  private socket: net.Socket | undefined;
  private buffer = "";

  onMessage: ((message: RawMessage) => void) | undefined;
  onConnect: (() => void) | undefined;
  onClose: (() => void) | undefined;
  onError: ((error: Error) => void) | undefined;

  get connected(): boolean {
    return this.socket !== undefined && !this.socket.connecting && !this.socket.destroyed;
  }

  connect(host: string, port: number): void {
    this.close();
    const socket = new net.Socket();
    this.socket = socket;
    this.buffer = "";
    socket.setEncoding("utf8");
    socket.setNoDelay(true);
    socket.on("data", (chunk: string) => this.handleData(chunk));
    socket.on("connect", () => {
      if (this.socket === socket) {
        this.onConnect?.();
      }
    });
    socket.on("error", (error: Error) => {
      if (this.socket === socket) {
        this.onError?.(error);
      }
    });
    socket.on("close", () => {
      if (this.socket === socket) {
        this.socket = undefined;
        this.onClose?.();
      }
    });
    socket.connect(port, host);
  }

  /** Promise-based connect; resolves once connected, rejects on first error. */
  connectAsync(host: string, port: number, timeoutMs = 5000): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let settled = false;
      const previousOnConnect = this.onConnect;
      const previousOnError = this.onError;
      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          this.onConnect = previousOnConnect;
          this.onError = previousOnError;
          reject(new Error(`Timed out connecting to ${host}:${port}`));
        }
      }, timeoutMs);
      this.onConnect = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          this.onConnect = previousOnConnect;
          this.onError = previousOnError;
          previousOnConnect?.();
          resolve();
        }
      };
      this.onError = (error) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          this.onConnect = previousOnConnect;
          this.onError = previousOnError;
          reject(error);
        } else {
          previousOnError?.(error);
        }
      };
      this.connect(host, port);
    });
  }

  send(message: ClientMessage | RawMessage): void {
    if (this.socket !== undefined && !this.socket.destroyed) {
      this.socket.write(JSON.stringify(message) + "\n");
    }
  }

  close(): void {
    const socket = this.socket;
    if (socket !== undefined) {
      // Detach first so closing does not fire our own onClose handler.
      this.socket = undefined;
      socket.removeAllListeners();
      socket.on("error", () => {
        /* swallow late errors */
      });
      socket.destroy();
    }
    this.buffer = "";
  }

  private handleData(chunk: string): void {
    this.buffer += chunk;
    for (;;) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        break;
      }
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (line.length === 0) {
        continue;
      }
      let parsed: unknown;
      try {
        parsed = JSON.parse(line);
      } catch {
        continue; // Ignore malformed lines.
      }
      if (typeof parsed === "object" && parsed !== null && typeof (parsed as RawMessage).type === "string") {
        this.onMessage?.(parsed as RawMessage);
      }
    }
  }
}
