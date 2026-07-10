/**
 * Lightweight, regex-based heuristics for inferring types in AngelScript
 * documents. Deliberately not a parser: good enough for completion, hover,
 * and signature help, and small enough to test in isolation.
 */

/** Words that can never be a user type or variable name in a declaration. */
const DECLARATION_BLACKLIST = new Set([
  "and", "abstract", "auto", "break", "case", "cast", "catch", "class",
  "const", "continue", "default", "do", "else", "enum", "explicit",
  "external", "fallthrough", "false", "final", "for", "from", "funcdef",
  "get", "if", "import", "in", "inout", "interface", "is", "mixin",
  "namespace", "not", "null", "or", "out", "override", "private",
  "property", "protected", "return", "set", "shared", "super", "switch",
  "this", "true", "try", "typedef", "while", "xor",
]);

export interface EnclosingClassInfo {
  name: string;
  base?: string;
}

/**
 * Finds the `class X : Base` header that encloses `offset` (heuristically:
 * the last class header declared before the offset).
 */
export function findEnclosingClass(text: string, offset: number): EnclosingClassInfo | undefined {
  const headerRe = /(?:^|\n)\s*class\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([A-Za-z_][A-Za-z0-9_]*))?/g;
  let result: EnclosingClassInfo | undefined;
  let match: RegExpExecArray | null;
  while ((match = headerRe.exec(text)) !== null) {
    if (match.index >= offset) {
      break;
    }
    result = { name: match[1], base: match[2] };
  }
  return result;
}

/**
 * Collects `Type name` declarations (locals, members, and function
 * parameters) from the document via regex. Returns variable name -> declared
 * type spelling (handle markers preserved, e.g. "Node@").
 */
export function collectVariableTypes(text: string): Map<string, string> {
  const variables = new Map<string, string>();
  const declRe =
    /\b([A-Za-z_][A-Za-z0-9_]*)\s*(@?)\s*(?:&\s*(?:in|out|inout)?\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*(?=[=;,)])/g;
  let match: RegExpExecArray | null;
  while ((match = declRe.exec(text)) !== null) {
    const typeName = match[1];
    const handle = match[2];
    const varName = match[3];
    if (DECLARATION_BLACKLIST.has(typeName) || DECLARATION_BLACKLIST.has(varName)) {
      continue;
    }
    if (typeName === "void") {
      continue;
    }
    if (!variables.has(varName)) {
      variables.set(varName, typeName + handle);
    }
  }
  return variables;
}

export interface DotContext {
  /** Identifier segments before the final dot, e.g. `a.b().` -> ["a", "b"]. */
  chain: string[];
  /** Parallel to `chain`: true when the segment was a call (`b()`). */
  calls: boolean[];
  /** The partially typed member after the final dot ("" when just typed `.`). */
  partial: string;
}

/**
 * Parses a receiver chain from the text immediately before the cursor, e.g.
 * `speed = this.get_parent().` -> chain ["this", "get_parent"].
 */
export function dotContext(linePrefix: string): DotContext | undefined {
  const match =
    /([A-Za-z_][A-Za-z0-9_]*(?:\s*\(\s*\))?(?:\s*\.\s*[A-Za-z_][A-Za-z0-9_]*(?:\s*\(\s*\))?)*)\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)?$/.exec(
      linePrefix
    );
  if (match === null) {
    return undefined;
  }
  const chain: string[] = [];
  const calls: boolean[] = [];
  for (const rawSegment of match[1].split(".")) {
    const segment = rawSegment.trim();
    if (/\(\s*\)$/.test(segment)) {
      chain.push(segment.replace(/\s*\(\s*\)$/, ""));
      calls.push(true);
    } else {
      chain.push(segment);
      calls.push(false);
    }
  }
  if (chain.length === 0) {
    return undefined;
  }
  return { chain, calls, partial: match[2] ?? "" };
}

/** Minimal view of the type database needed to resolve receiver chains. */
export interface TypeResolver {
  classExists(name: string): boolean;
  /** Type of `className.memberName` (property type or method return type). */
  memberType(className: string, memberName: string, isCall: boolean): string | undefined;
}

/**
 * Resolves the type of a receiver chain: the first segment is looked up as
 * `this`, a declared variable, a class name (static access), or a member of
 * the enclosing class; later segments are member accesses on the result.
 * Returns a type spelling (possibly with handle markers) or undefined.
 */
export function resolveChainType(
  context: Pick<DotContext, "chain" | "calls">,
  variables: Map<string, string>,
  enclosing: EnclosingClassInfo | undefined,
  resolver: TypeResolver
): string | undefined {
  let currentType: string | undefined;
  for (let i = 0; i < context.chain.length; i++) {
    const segment = context.chain[i];
    const isCall = context.calls[i];
    if (i === 0) {
      if (segment === "this") {
        currentType = enclosing?.name;
      } else if (!isCall && variables.has(segment)) {
        currentType = variables.get(segment);
      } else if (!isCall && resolver.classExists(segment)) {
        currentType = segment;
      } else if (enclosing !== undefined) {
        currentType = resolver.memberType(enclosing.name, segment, isCall);
      }
    } else {
      currentType = resolver.memberType(currentType as string, segment, isCall);
    }
    if (currentType === undefined) {
      return undefined;
    }
  }
  return currentType;
}

export interface CallContext {
  /** Name of the function being called. */
  name: string;
  /** Text before the callee name (used to detect a receiver chain). */
  receiverPrefix: string;
  /** Zero-based index of the argument being typed. */
  activeParameter: number;
}

/**
 * Finds the innermost unclosed call in `linePrefix`, for signature help.
 * String literals are skipped; nesting of () and [] is respected.
 */
export function findCallContext(linePrefix: string): CallContext | undefined {
  let depth = 0;
  let commas = 0;
  let inString: string | undefined;
  // Walk backwards; a rough scan is fine for single-line call expressions.
  for (let i = linePrefix.length - 1; i >= 0; i--) {
    const ch = linePrefix.charAt(i);
    if (inString !== undefined) {
      if (ch === inString && linePrefix.charAt(i - 1) !== "\\") {
        inString = undefined;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      inString = ch;
      continue;
    }
    if (ch === ")" || ch === "]") {
      depth++;
    } else if (ch === "(") {
      if (depth === 0) {
        const before = linePrefix.slice(0, i);
        const nameMatch = /([A-Za-z_][A-Za-z0-9_]*)\s*$/.exec(before);
        if (nameMatch === null) {
          return undefined;
        }
        return {
          name: nameMatch[1],
          receiverPrefix: before.slice(0, before.length - nameMatch[0].length),
          activeParameter: commas,
        };
      }
      depth--;
    } else if (ch === "[") {
      if (depth > 0) {
        depth--;
      }
    } else if (ch === "," && depth === 0) {
      commas++;
    }
  }
  return undefined;
}

export interface WordAtResult {
  word: string;
  start: number;
  end: number;
}

/** The identifier at (or immediately before) `offset`, with its text range. */
export function wordAt(text: string, offset: number): WordAtResult | undefined {
  const isWordChar = (ch: string): boolean => /[A-Za-z0-9_]/.test(ch);
  let start = offset;
  let end = offset;
  while (start > 0 && isWordChar(text.charAt(start - 1))) {
    start--;
  }
  while (end < text.length && isWordChar(text.charAt(end))) {
    end++;
  }
  if (start === end) {
    return undefined;
  }
  const word = text.slice(start, end);
  if (!/^[A-Za-z_]/.test(word)) {
    return undefined;
  }
  return { word, start, end };
}
