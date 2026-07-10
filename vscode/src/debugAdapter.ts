/**
 * Debug adapter for Godot AngelScript. Launched by VS Code as a standalone
 * node program (see the "debuggers" contribution in package.json) and speaks
 * DAP on stdio while talking the docs/PROTOCOL.md line-JSON protocol to the
 * engine's tooling server over TCP.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import {
  ContinuedEvent,
  DebugSession,
  InitializedEvent,
  Scope,
  Source,
  StackFrame,
  StoppedEvent,
  TerminatedEvent,
  Thread,
} from "@vscode/debugadapter";
import { DebugProtocol } from "@vscode/debugprotocol";
import {
  BreakpointsSetMessage,
  DEFAULT_HOST,
  DEFAULT_PORT,
  LineJsonSocket,
  RawMessage,
  StackMessage,
  StoppedMessage,
  VariableScopeName,
  VariablesMessage,
  diskPathToResPath,
  makeHello,
  resPathToDiskPath,
} from "./common/protocol";

const THREAD_ID = 1;
const REPLY_TIMEOUT_MS = 5000;

interface AttachArguments extends DebugProtocol.AttachRequestArguments {
  port?: number;
  hostname?: string;
  projectRoot?: string;
}

interface Waiter {
  predicate: (message: RawMessage) => boolean;
  resolve: (message: RawMessage) => void;
  timer: NodeJS.Timeout;
}

function findProjectGodotDir(root: string, maxDepth: number): string | undefined {
  const queue: Array<{ dir: string; depth: number }> = [{ dir: root, depth: 0 }];
  while (queue.length > 0) {
    const { dir, depth } = queue.shift() as { dir: string; depth: number };
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    if (entries.some((entry) => entry.isFile() && entry.name === "project.godot")) {
      return dir;
    }
    if (depth >= maxDepth) {
      continue;
    }
    for (const entry of entries) {
      if (entry.isDirectory() && !entry.name.startsWith(".") && entry.name !== "node_modules") {
        queue.push({ dir: path.join(dir, entry.name), depth: depth + 1 });
      }
    }
  }
  return undefined;
}

class GodotAngelscriptDebugSession extends DebugSession {
  private readonly socket = new LineJsonSocket();
  private projectRoot = "";
  private waiters: Waiter[] = [];
  private attached = false;

  constructor() {
    super();
    this.setDebuggerLinesStartAt1(true);
    this.setDebuggerColumnsStartAt1(true);
    this.socket.onMessage = (message) => this.handleEngineMessage(message);
    this.socket.onClose = () => {
      if (this.attached) {
        this.attached = false;
        this.sendEvent(new TerminatedEvent());
      }
    };
  }

  // -------------------------------------------------------------------------
  // Engine message plumbing
  // -------------------------------------------------------------------------

  private waitFor(predicate: (message: RawMessage) => boolean, timeoutMs = REPLY_TIMEOUT_MS): Promise<RawMessage> {
    return new Promise<RawMessage>((resolve, reject) => {
      const waiter: Waiter = {
        predicate,
        resolve,
        timer: setTimeout(() => {
          this.waiters = this.waiters.filter((w) => w !== waiter);
          reject(new Error("Timed out waiting for engine reply"));
        }, timeoutMs),
      };
      this.waiters.push(waiter);
    });
  }

  private handleEngineMessage(message: RawMessage): void {
    for (let i = 0; i < this.waiters.length; i++) {
      const waiter = this.waiters[i];
      let matches = false;
      try {
        matches = waiter.predicate(message);
      } catch {
        matches = false;
      }
      if (matches) {
        clearTimeout(waiter.timer);
        this.waiters.splice(i, 1);
        waiter.resolve(message);
        return;
      }
    }

    switch (message.type) {
      case "stopped": {
        const stopped = message as unknown as StoppedMessage;
        const event = new StoppedEvent(stopped.reason, THREAD_ID);
        if (stopped.reason === "exception" && typeof stopped.text === "string" && stopped.text.length > 0) {
          (event.body as DebugProtocol.StoppedEvent["body"]).text = stopped.text;
          (event.body as DebugProtocol.StoppedEvent["body"]).description = stopped.text;
        }
        this.sendEvent(event);
        break;
      }
      case "continued": {
        this.sendEvent(new ContinuedEvent(THREAD_ID, true));
        break;
      }
      case "exited": {
        this.attached = false;
        this.sendEvent(new TerminatedEvent());
        break;
      }
      default:
        // Diagnostics / type DB traffic and unknown messages are ignored here.
        break;
    }
  }

  // -------------------------------------------------------------------------
  // DAP requests
  // -------------------------------------------------------------------------

  protected initializeRequest(
    response: DebugProtocol.InitializeResponse,
    _args: DebugProtocol.InitializeRequestArguments
  ): void {
    response.body = response.body ?? {};
    response.body.supportsConfigurationDoneRequest = true;
    response.body.supportsEvaluateForHovers = true;
    this.sendResponse(response);
  }

  protected async attachRequest(
    response: DebugProtocol.AttachResponse,
    args: AttachArguments
  ): Promise<void> {
    const host = args.hostname ?? DEFAULT_HOST;
    const port = args.port ?? DEFAULT_PORT;
    this.projectRoot = this.resolveProjectRoot(args.projectRoot);
    try {
      await this.socket.connectAsync(host, port);
      this.socket.send(makeHello("vscode-dap"));
      await this.waitFor((m) => m.type === "welcome");
      this.attached = true;
      this.sendResponse(response);
      // Ask the client for breakpoints etc. now that the engine is reachable.
      this.sendEvent(new InitializedEvent());
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      this.sendErrorResponse(response, {
        id: 1001,
        format: `Failed to attach to Godot at ${host}:${port}: ${detail}. Make sure the game is running with the AngelScript GDExtension tooling server enabled.`,
        showUser: true,
      });
    }
  }

  protected configurationDoneRequest(
    response: DebugProtocol.ConfigurationDoneResponse,
    _args: DebugProtocol.ConfigurationDoneArguments
  ): void {
    this.sendResponse(response);
  }

  protected async setBreakPointsRequest(
    response: DebugProtocol.SetBreakpointsResponse,
    args: DebugProtocol.SetBreakpointsArguments
  ): Promise<void> {
    const clientLines = args.breakpoints?.map((bp) => bp.line) ?? args.lines ?? [];
    const debuggerLines = clientLines.map((line) => this.convertClientLineToDebugger(line));

    const sourcePath = args.source.path;
    const resPath = sourcePath !== undefined ? diskPathToResPath(sourcePath, this.projectRoot) : undefined;
    if (resPath === undefined) {
      response.body = {
        breakpoints: clientLines.map((line) => ({
          verified: false,
          line,
          message: "File is outside the Godot project root; cannot map to a res:// path.",
        })),
      };
      this.sendResponse(response);
      return;
    }

    this.socket.send({ type: "set_breakpoints", path: resPath, lines: debuggerLines });

    let verified = new Set<number>();
    try {
      const reply = (await this.waitFor(
        (m) => m.type === "breakpoints_set" && (m as unknown as BreakpointsSetMessage).path === resPath
      )) as unknown as BreakpointsSetMessage;
      verified = new Set(reply.verified);
    } catch {
      // No confirmation from the engine; report all breakpoints unverified.
    }

    response.body = {
      breakpoints: debuggerLines.map((line) => ({
        verified: verified.has(line),
        line: this.convertDebuggerLineToClient(line),
      })),
    };
    this.sendResponse(response);
  }

  protected threadsRequest(response: DebugProtocol.ThreadsResponse): void {
    response.body = { threads: [new Thread(THREAD_ID, "Main")] };
    this.sendResponse(response);
  }

  protected async stackTraceRequest(
    response: DebugProtocol.StackTraceResponse,
    args: DebugProtocol.StackTraceArguments
  ): Promise<void> {
    try {
      this.socket.send({ type: "request_stack" });
      const reply = (await this.waitFor((m) => m.type === "stack")) as unknown as StackMessage;
      const startFrame = args.startFrame ?? 0;
      const levels = args.levels !== undefined && args.levels > 0 ? args.levels : reply.frames.length;
      const frames = reply.frames.slice(startFrame, startFrame + levels).map((frame) => {
        const diskPath = resPathToDiskPath(frame.path, this.projectRoot);
        const source =
          diskPath !== undefined
            ? new Source(path.basename(diskPath), this.convertDebuggerPathToClient(diskPath))
            : new Source(frame.path);
        return new StackFrame(frame.id, frame.func, source, this.convertDebuggerLineToClient(frame.line), 1);
      });
      response.body = { stackFrames: frames, totalFrames: reply.frames.length };
      this.sendResponse(response);
    } catch (error) {
      this.sendErrorResponse(response, {
        id: 1002,
        format: `Failed to fetch the call stack: ${error instanceof Error ? error.message : String(error)}`,
      });
    }
  }

  protected scopesRequest(response: DebugProtocol.ScopesResponse, args: DebugProtocol.ScopesArguments): void {
    response.body = {
      scopes: [
        new Scope("Locals", this.encodeVariablesReference(args.frameId, "locals"), false),
        new Scope("Members", this.encodeVariablesReference(args.frameId, "members"), false),
      ],
    };
    this.sendResponse(response);
  }

  protected async variablesRequest(
    response: DebugProtocol.VariablesResponse,
    args: DebugProtocol.VariablesArguments
  ): Promise<void> {
    const { frame, scope } = this.decodeVariablesReference(args.variablesReference);
    try {
      this.socket.send({ type: "request_variables", frame, scope });
      const reply = (await this.waitFor((m) => {
        if (m.type !== "variables") {
          return false;
        }
        const vars = m as unknown as VariablesMessage;
        return vars.frame === frame && vars.scope === scope;
      })) as unknown as VariablesMessage;
      response.body = {
        variables: reply.variables.map((variable) => ({
          name: variable.name,
          value: variable.value,
          type: variable.type,
          variablesReference: 0,
        })),
      };
      this.sendResponse(response);
    } catch {
      response.body = { variables: [] };
      this.sendResponse(response);
    }
  }

  protected continueRequest(response: DebugProtocol.ContinueResponse, _args: DebugProtocol.ContinueArguments): void {
    this.socket.send({ type: "continue" });
    response.body = { allThreadsContinued: true };
    this.sendResponse(response);
  }

  protected nextRequest(response: DebugProtocol.NextResponse, _args: DebugProtocol.NextArguments): void {
    this.socket.send({ type: "step_over" });
    this.sendResponse(response);
  }

  protected stepInRequest(response: DebugProtocol.StepInResponse, _args: DebugProtocol.StepInArguments): void {
    this.socket.send({ type: "step_in" });
    this.sendResponse(response);
  }

  protected stepOutRequest(response: DebugProtocol.StepOutResponse, _args: DebugProtocol.StepOutArguments): void {
    this.socket.send({ type: "step_out" });
    this.sendResponse(response);
  }

  protected pauseRequest(response: DebugProtocol.PauseResponse, _args: DebugProtocol.PauseArguments): void {
    this.socket.send({ type: "pause" });
    this.sendResponse(response);
  }

  protected async evaluateRequest(
    response: DebugProtocol.EvaluateResponse,
    args: DebugProtocol.EvaluateArguments
  ): Promise<void> {
    const frame = args.frameId ?? 0;
    try {
      this.socket.send({ type: "request_evaluate", frame, expression: args.expression });
      // Protocol quirk: the documented evaluate reply reuses the "type" key
      // for the value type ({"type": "evaluate", "value": "600", "type":
      // "double"}); after JSON parsing the last key wins, so the envelope
      // type may be the value's type name. Match on the "value" field shape.
      const reply = await this.waitFor((m) => {
        if (m.type === "evaluate") {
          return true;
        }
        return (
          typeof m.value === "string" &&
          m.variables === undefined &&
          m.frames === undefined &&
          m.items === undefined
        );
      });
      const valueType = reply.type !== "evaluate" ? reply.type : undefined;
      response.body = {
        result: typeof reply.value === "string" ? reply.value : String(reply.value ?? ""),
        type: valueType,
        variablesReference: 0,
      };
      this.sendResponse(response);
    } catch (error) {
      this.sendErrorResponse(response, {
        id: 1003,
        format: `Evaluation failed: ${error instanceof Error ? error.message : String(error)}`,
      });
    }
  }

  protected disconnectRequest(
    response: DebugProtocol.DisconnectResponse,
    _args: DebugProtocol.DisconnectArguments
  ): void {
    this.attached = false;
    for (const waiter of this.waiters) {
      clearTimeout(waiter.timer);
    }
    this.waiters = [];
    this.socket.close();
    this.sendResponse(response);
  }

  // -------------------------------------------------------------------------
  // Helpers
  // -------------------------------------------------------------------------

  /** variablesReference must be > 0; pack frame id and scope into one int. */
  private encodeVariablesReference(frameId: number, scope: VariableScopeName): number {
    return frameId * 2 + (scope === "locals" ? 1 : 2);
  }

  private decodeVariablesReference(reference: number): { frame: number; scope: VariableScopeName } {
    const frame = Math.floor((reference - 1) / 2);
    const scope: VariableScopeName = (reference - 1) % 2 === 0 ? "locals" : "members";
    return { frame, scope };
  }

  private resolveProjectRoot(configured: string | undefined): string {
    if (configured !== undefined && configured.length > 0) {
      return path.resolve(configured);
    }
    return findProjectGodotDir(process.cwd(), 3) ?? process.cwd();
  }
}

DebugSession.run(GodotAngelscriptDebugSession);
