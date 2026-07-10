/**
 * Godot AngelScript language server.
 *
 * Connects to the engine's TCP tooling server for the type database and
 * diagnostics; falls back gracefully to document-local heuristics when the
 * engine is offline.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
import {
  CompletionItem,
  CompletionItemKind,
  Diagnostic,
  DiagnosticSeverity,
  Hover,
  InitializeParams,
  InitializeResult,
  Location,
  MarkupKind,
  ParameterInformation,
  ProposedFeatures,
  Range,
  SignatureHelp,
  SignatureInformation,
  TextDocumentSyncKind,
  TextDocuments,
  createConnection,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";
import {
  DEFAULT_HOST,
  DEFAULT_PORT,
  DiagnosticsMessage,
  RES_PREFIX,
  TypeDbMethod,
  resPathToDiskPath,
} from "../common/protocol";
import { EngineConnection } from "./engineConnection";
import { MemberInfo, TypeDatabase, methodSignature, normalizeTypeName } from "./typedb";
import {
  EnclosingClassInfo,
  TypeResolver,
  collectVariableTypes,
  dotContext,
  findCallContext,
  findEnclosingClass,
  resolveChainType,
  wordAt,
} from "./heuristics";

interface Settings {
  enginePort: number;
  engineHost: string;
  projectRoot: string;
}

const DEFAULT_SETTINGS: Settings = {
  enginePort: DEFAULT_PORT,
  engineHost: DEFAULT_HOST,
  projectRoot: "",
};

const STATEMENT_KEYWORDS = [
  "class", "interface", "enum", "namespace", "funcdef", "typedef", "import",
  "from", "mixin", "if", "else", "for", "while", "do", "switch", "case",
  "default", "break", "continue", "return", "try", "catch", "const", "final",
  "override", "abstract", "shared", "external", "property", "get", "set",
  "private", "protected", "auto", "void", "bool", "int", "uint", "float",
  "double", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32",
  "uint64", "true", "false", "null", "this", "super", "cast", "is", "and",
  "or", "not", "xor", "in", "out", "inout",
];

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

const typeDb = new TypeDatabase();
const engine = new EngineConnection(typeDb, "vscode-lsp");

let settings: Settings = { ...DEFAULT_SETTINGS };
let workspaceRoots: string[] = [];
let projectRoot = "";
const publishedDiagnosticUris = new Set<string>();

// ---------------------------------------------------------------------------
// Project root discovery (folder containing project.godot)
// ---------------------------------------------------------------------------

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

function resolveProjectRoot(): string {
  if (settings.projectRoot.length > 0) {
    if (path.isAbsolute(settings.projectRoot)) {
      return settings.projectRoot;
    }
    if (workspaceRoots.length > 0) {
      return path.join(workspaceRoots[0], settings.projectRoot);
    }
    return path.resolve(settings.projectRoot);
  }
  for (const root of workspaceRoots) {
    const found = findProjectGodotDir(root, 3);
    if (found !== undefined) {
      return found;
    }
  }
  return workspaceRoots[0] ?? "";
}

function resPathToUri(resPath: string): string | undefined {
  const diskPath = resPathToDiskPath(resPath, projectRoot);
  if (diskPath === undefined) {
    return undefined;
  }
  return pathToFileURL(diskPath).href;
}

// ---------------------------------------------------------------------------
// Engine connection wiring
// ---------------------------------------------------------------------------

engine.onLog = (message) => connection.console.log(`[engine] ${message}`);

engine.onStatusChange = (connected) => {
  if (!connected) {
    connection.console.log("[engine] offline; falling back to document heuristics");
  }
};

engine.onDiagnostics = (message: DiagnosticsMessage) => {
  const uri = resPathToUri(message.path);
  if (uri === undefined) {
    connection.console.warn(`[engine] cannot map diagnostics path ${message.path} (projectRoot: "${projectRoot}")`);
    return;
  }
  const openDoc = documents.get(uri);
  const diagnostics: Diagnostic[] = message.items.map((item) => {
    const line = Math.max(0, item.line - 1);
    const character = Math.max(0, item.column - 1);
    let endCharacter = character + 1;
    if (openDoc !== undefined) {
      const lineEnd = openDoc.offsetAt({ line: line + 1, character: 0 }) - openDoc.offsetAt({ line, character: 0 });
      endCharacter = Math.max(endCharacter, lineEnd - 1);
    } else {
      endCharacter = character + 1000; // Clamped by the client to the line length.
    }
    const severity =
      item.severity === "error"
        ? DiagnosticSeverity.Error
        : item.severity === "warning"
          ? DiagnosticSeverity.Warning
          : DiagnosticSeverity.Information;
    return {
      range: Range.create(line, character, line, endCharacter),
      severity,
      message: item.message,
      source: "godot-angelscript",
    };
  });
  publishedDiagnosticUris.add(uri);
  void connection.sendDiagnostics({ uri, diagnostics });
};

// ---------------------------------------------------------------------------
// Heuristic helpers bound to the current document
// ---------------------------------------------------------------------------

const resolver: TypeResolver = {
  classExists: (name) => typeDb.get(name) !== undefined,
  memberType: (className, memberName, isCall) => typeDb.memberType(normalizeTypeName(className), memberName, isCall),
};

interface DocumentContext {
  text: string;
  offset: number;
  linePrefix: string;
  enclosing: EnclosingClassInfo | undefined;
  variables: Map<string, string>;
}

function documentContext(document: TextDocument, position: { line: number; character: number }): DocumentContext {
  const text = document.getText();
  const offset = document.offsetAt(position);
  const lineStart = document.offsetAt({ line: position.line, character: 0 });
  return {
    text,
    offset,
    linePrefix: text.slice(lineStart, offset),
    enclosing: findEnclosingClass(text, offset),
    variables: collectVariableTypes(text),
  };
}

function memberDetail(member: MemberInfo): string {
  switch (member.kind) {
    case "method":
      return methodSignature(member.method);
    case "property":
      return `${member.property.type} ${member.property.name}`;
    case "signal": {
      const args = member.signal.args.map((a) => `${a.type} ${a.name}`).join(", ");
      return `signal ${member.signal.name}(${args})`;
    }
    case "constant":
      return `const ${member.constant.name} = ${member.constant.value}`;
    case "enum":
      return `enum ${member.enumInfo.name}`;
  }
}

function memberCompletionKind(member: MemberInfo): CompletionItemKind {
  switch (member.kind) {
    case "method":
      return CompletionItemKind.Method;
    case "property":
      return CompletionItemKind.Field;
    case "signal":
      return CompletionItemKind.Event;
    case "constant":
      return CompletionItemKind.Constant;
    case "enum":
      return CompletionItemKind.Enum;
  }
}

function memberName(member: MemberInfo): string {
  switch (member.kind) {
    case "method":
      return member.method.name;
    case "property":
      return member.property.name;
    case "signal":
      return member.signal.name;
    case "constant":
      return member.constant.name;
    case "enum":
      return member.enumInfo.name;
  }
}

// ---------------------------------------------------------------------------
// LSP lifecycle
// ---------------------------------------------------------------------------

connection.onInitialize((params: InitializeParams): InitializeResult => {
  workspaceRoots = (params.workspaceFolders ?? [])
    .map((folder) => {
      try {
        return fileURLToPath(folder.uri);
      } catch {
        return "";
      }
    })
    .filter((p) => p.length > 0);

  const initSettings = (params.initializationOptions as { settings?: Partial<Settings> } | undefined)?.settings;
  if (initSettings !== undefined) {
    settings = { ...DEFAULT_SETTINGS, ...initSettings };
  }

  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: { triggerCharacters: ["."] },
      hoverProvider: true,
      definitionProvider: true,
      signatureHelpProvider: { triggerCharacters: ["(", ","] },
    },
  };
});

connection.onInitialized(() => {
  projectRoot = resolveProjectRoot();
  connection.console.log(`[server] project root: ${projectRoot || "(none)"}`);
  engine.start(settings.engineHost, settings.enginePort);
});

connection.onDidChangeConfiguration((change) => {
  const incoming = (change.settings as { godotAngelscript?: Partial<Settings> } | undefined)?.godotAngelscript;
  settings = { ...DEFAULT_SETTINGS, ...incoming };
  projectRoot = resolveProjectRoot();
  engine.updateTarget(settings.engineHost, settings.enginePort);
});

connection.onShutdown(() => {
  engine.stop();
});

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

connection.onCompletion((params): CompletionItem[] => {
  const document = documents.get(params.textDocument.uri);
  if (document === undefined) {
    return [];
  }
  const ctx = documentContext(document, params.position);

  const dot = dotContext(ctx.linePrefix);
  if (dot !== undefined) {
    const receiverType = resolveChainType(dot, ctx.variables, ctx.enclosing, resolver);
    if (receiverType === undefined) {
      return [];
    }
    const className = normalizeTypeName(receiverType);
    return typeDb.membersOf(className).map((member) => ({
      label: memberName(member),
      kind: memberCompletionKind(member),
      detail: memberDetail(member),
      documentation: member.owner !== className ? `From ${member.owner}` : undefined,
    }));
  }

  const items: CompletionItem[] = [];
  const seen = new Set<string>();
  const add = (item: CompletionItem): void => {
    if (!seen.has(item.label)) {
      seen.add(item.label);
      items.push(item);
    }
  };

  for (const keyword of STATEMENT_KEYWORDS) {
    add({ label: keyword, kind: CompletionItemKind.Keyword });
  }
  for (const cls of typeDb.allClasses()) {
    add({
      label: cls.name,
      kind: CompletionItemKind.Class,
      detail: cls.base !== undefined ? `class ${cls.name} : ${cls.base}` : `class ${cls.name}`,
    });
  }
  if (ctx.enclosing !== undefined) {
    for (const member of typeDb.membersOf(ctx.enclosing.name)) {
      if (member.kind === "method") {
        add({
          label: member.method.name,
          kind: CompletionItemKind.Method,
          detail: methodSignature(member.method),
          documentation: `From ${member.owner}`,
        });
      } else if (member.kind === "property") {
        add({
          label: member.property.name,
          kind: CompletionItemKind.Field,
          detail: `${member.property.type} ${member.property.name}`,
          documentation: `From ${member.owner}`,
        });
      }
    }
  }
  for (const [name, type] of ctx.variables) {
    add({ label: name, kind: CompletionItemKind.Variable, detail: `${type} ${name}` });
  }
  return items;
});

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

function hoverMarkdown(code: string, extra?: string): Hover {
  let value = "```angelscript\n" + code + "\n```";
  if (extra !== undefined && extra.length > 0) {
    value += "\n\n" + extra;
  }
  return { contents: { kind: MarkupKind.Markdown, value } };
}

connection.onHover((params): Hover | null => {
  const document = documents.get(params.textDocument.uri);
  if (document === undefined) {
    return null;
  }
  const ctx = documentContext(document, params.position);
  const word = wordAt(ctx.text, ctx.offset);
  if (word === undefined) {
    return null;
  }
  const lineStart = document.offsetAt({ line: params.position.line, character: 0 });
  const prefixBeforeWord = ctx.text.slice(lineStart, word.start);

  // Member access: `receiver.word`
  if (/\.\s*$/.test(prefixBeforeWord)) {
    const dot = dotContext(prefixBeforeWord);
    if (dot !== undefined) {
      const receiverType = resolveChainType(dot, ctx.variables, ctx.enclosing, resolver);
      if (receiverType !== undefined) {
        const member = typeDb.findMember(normalizeTypeName(receiverType), word.word);
        if (member !== undefined) {
          return hoverMarkdown(memberDetail(member), `Member of \`${member.owner}\``);
        }
      }
    }
    return null;
  }

  // Class names.
  const cls = typeDb.get(word.word);
  if (cls !== undefined) {
    const header = cls.base !== undefined ? `class ${cls.name} : ${cls.base}` : `class ${cls.name}`;
    const origin = cls.native ? "Native class" : `Script class (\`${cls.path ?? "?"}\`)`;
    return hoverMarkdown(header, origin);
  }

  // Declared variables.
  const varType = ctx.variables.get(word.word);
  if (varType !== undefined && word.word !== "this") {
    return hoverMarkdown(`${varType} ${word.word}`);
  }

  // Members of the enclosing class (implicit this).
  if (ctx.enclosing !== undefined) {
    const member = typeDb.findMember(ctx.enclosing.name, word.word);
    if (member !== undefined) {
      return hoverMarkdown(memberDetail(member), `Member of \`${member.owner}\``);
    }
  }
  return null;
});

// ---------------------------------------------------------------------------
// Go to definition
// ---------------------------------------------------------------------------

function searchDocumentForDeclaration(document: TextDocument, word: string): Location | undefined {
  const text = document.getText();
  const escaped = word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const patterns = [
    // class / enum / interface / namespace declaration
    new RegExp(`\\b(?:class|enum|interface|namespace|funcdef)\\s+(${escaped})\\b`),
    // function declaration: "ReturnType name("
    new RegExp(`\\b[A-Za-z_][A-Za-z0-9_<>@,\\s]*[@&]?\\s+(${escaped})\\s*\\(`),
    // variable / property declaration: "Type name =" | "Type name;"
    new RegExp(`\\b[A-Za-z_][A-Za-z0-9_<>@]*\\s*@?\\s+(${escaped})\\s*[=;,)]`),
  ];
  for (const pattern of patterns) {
    const match = pattern.exec(text);
    if (match !== null && match[1] !== undefined) {
      const start = match.index + match[0].indexOf(match[1]);
      return Location.create(document.uri, Range.create(document.positionAt(start), document.positionAt(start + word.length)));
    }
  }
  return undefined;
}

connection.onDefinition((params): Location | null => {
  const document = documents.get(params.textDocument.uri);
  if (document === undefined) {
    return null;
  }
  const ctx = documentContext(document, params.position);
  const word = wordAt(ctx.text, ctx.offset);
  if (word === undefined) {
    return null;
  }

  // Script classes carry their file path and declaration line in the DB.
  const cls = typeDb.get(word.word);
  if (cls !== undefined && !cls.native && cls.path !== undefined && cls.path.startsWith(RES_PREFIX)) {
    const uri = resPathToUri(cls.path);
    if (uri !== undefined) {
      const line = Math.max(0, (cls.line ?? 1) - 1);
      return Location.create(uri, Range.create(line, 0, line, 0));
    }
  }

  // Members and locals: regex search over open documents, current one first.
  const local = searchDocumentForDeclaration(document, word.word);
  if (local !== undefined) {
    return local;
  }
  for (const other of documents.all()) {
    if (other.uri === document.uri) {
      continue;
    }
    const found = searchDocumentForDeclaration(other, word.word);
    if (found !== undefined) {
      return found;
    }
  }
  return null;
});

// ---------------------------------------------------------------------------
// Signature help
// ---------------------------------------------------------------------------

function signatureFromMethod(method: TypeDbMethod, activeParameter: number): SignatureHelp {
  const parameters: ParameterInformation[] = method.args.map((arg) => {
    let label = `${arg.type} ${arg.name}`;
    if (arg.default !== undefined && arg.default !== null) {
      label += ` = ${arg.default}`;
    }
    return ParameterInformation.create(label);
  });
  const signature = SignatureInformation.create(methodSignature(method), undefined, ...parameters);
  const maxIndex = method.vararg === true ? activeParameter : Math.max(0, method.args.length - 1);
  return {
    signatures: [signature],
    activeSignature: 0,
    activeParameter: Math.min(activeParameter, maxIndex),
  };
}

connection.onSignatureHelp((params): SignatureHelp | null => {
  const document = documents.get(params.textDocument.uri);
  if (document === undefined) {
    return null;
  }
  const ctx = documentContext(document, params.position);
  const call = findCallContext(ctx.linePrefix);
  if (call === undefined) {
    return null;
  }

  // Receiver chain before the callee, e.g. "player.get_node(".
  if (/\.\s*$/.test(call.receiverPrefix)) {
    const dot = dotContext(call.receiverPrefix);
    if (dot !== undefined) {
      const receiverType = resolveChainType(dot, ctx.variables, ctx.enclosing, resolver);
      if (receiverType !== undefined) {
        const found = typeDb.findMethod(normalizeTypeName(receiverType), call.name);
        if (found !== undefined) {
          return signatureFromMethod(found.method, call.activeParameter);
        }
      }
    }
    return null;
  }

  // Bare call: method of the enclosing class (or its bases).
  if (ctx.enclosing !== undefined) {
    const found = typeDb.findMethod(ctx.enclosing.name, call.name);
    if (found !== undefined) {
      return signatureFromMethod(found.method, call.activeParameter);
    }
  }
  return null;
});

// ---------------------------------------------------------------------------

documents.listen(connection);
connection.listen();
