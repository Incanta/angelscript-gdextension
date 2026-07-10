/**
 * In-memory symbol database built from the engine's type_db_class stream.
 * Deliberately dependency-free (besides the protocol types) so it can be
 * exercised by the protocol smoke test outside of VS Code.
 */

import type {
  TypeDbClassMessage,
  TypeDbConstant,
  TypeDbEnum,
  TypeDbMethod,
  TypeDbProperty,
  TypeDbSignal,
} from "../common/protocol";

export interface ClassInfo {
  name: string;
  base?: string;
  native: boolean;
  /** res:// path for script classes (native: false). */
  path?: string;
  /** 1-based line of the class declaration for script classes. */
  line?: number;
  methods: TypeDbMethod[];
  properties: TypeDbProperty[];
  signals: TypeDbSignal[];
  constants: TypeDbConstant[];
  enums: TypeDbEnum[];
}

export type MemberInfo =
  | { kind: "method"; owner: string; method: TypeDbMethod }
  | { kind: "property"; owner: string; property: TypeDbProperty }
  | { kind: "signal"; owner: string; signal: TypeDbSignal }
  | { kind: "constant"; owner: string; constant: TypeDbConstant }
  | { kind: "enum"; owner: string; enumInfo: TypeDbEnum };

/**
 * Normalizes an AngelScript type spelling to a bare class name usable as a
 * database key: strips const, handle (@) and reference (&) markers, and
 * template arguments ("array<int>" -> "array").
 */
export function normalizeTypeName(type: string): string {
  let name = type.trim();
  if (name.startsWith("const ")) {
    name = name.slice("const ".length).trim();
  }
  name = name.replace(/\s*&\s*(in|out|inout)?\s*$/, "");
  name = name.replace(/[@&\s]+$/, "");
  const angleBracket = name.indexOf("<");
  if (angleBracket > 0) {
    name = name.slice(0, angleBracket);
  }
  return name.trim();
}

/** Renders a method as a human-readable signature string. */
export function methodSignature(method: TypeDbMethod): string {
  const args = method.args.map((arg) => {
    let text = `${arg.type} ${arg.name}`;
    if (arg.default !== undefined && arg.default !== null) {
      text += ` = ${arg.default}`;
    }
    return text;
  });
  if (method.vararg === true) {
    args.push("...");
  }
  const prefix = method.static === true ? "static " : "";
  return `${prefix}${method.return} ${method.name}(${args.join(", ")})`;
}

export class TypeDatabase {
  private readonly classes = new Map<string, ClassInfo>();

  get count(): number {
    return this.classes.size;
  }

  clear(): void {
    this.classes.clear();
  }

  /** Ingests a single type_db_class message (initial stream or script re-send). */
  ingestClass(message: TypeDbClassMessage): ClassInfo {
    const info: ClassInfo = {
      name: message.name,
      base: message.base,
      native: message.native,
      path: message.path,
      line: message.line,
      methods: message.methods ?? [],
      properties: message.properties ?? [],
      signals: message.signals ?? [],
      constants: message.constants ?? [],
      enums: message.enums ?? [],
    };
    this.classes.set(info.name, info);
    return info;
  }

  get(name: string): ClassInfo | undefined {
    return this.classes.get(name);
  }

  /** Looks a class up by any AngelScript type spelling (e.g. "Node@"). */
  resolve(typeName: string): ClassInfo | undefined {
    return this.classes.get(normalizeTypeName(typeName));
  }

  allClasses(): ClassInfo[] {
    return [...this.classes.values()];
  }

  /** The class and all its known bases, most derived first. Cycle-safe. */
  baseChain(name: string): ClassInfo[] {
    const chain: ClassInfo[] = [];
    const seen = new Set<string>();
    let current = this.resolve(name);
    while (current !== undefined && !seen.has(current.name)) {
      seen.add(current.name);
      chain.push(current);
      current = current.base !== undefined ? this.classes.get(current.base) : undefined;
    }
    return chain;
  }

  /** Finds a member (method/property/signal/constant/enum) walking the base chain. */
  findMember(className: string, memberName: string): MemberInfo | undefined {
    for (const cls of this.baseChain(className)) {
      const method = cls.methods.find((m) => m.name === memberName);
      if (method !== undefined) {
        return { kind: "method", owner: cls.name, method };
      }
      const property = cls.properties.find((p) => p.name === memberName);
      if (property !== undefined) {
        return { kind: "property", owner: cls.name, property };
      }
      const signal = cls.signals.find((s) => s.name === memberName);
      if (signal !== undefined) {
        return { kind: "signal", owner: cls.name, signal };
      }
      const constant = cls.constants.find((c) => c.name === memberName);
      if (constant !== undefined) {
        return { kind: "constant", owner: cls.name, constant };
      }
      const enumInfo = cls.enums.find((e) => e.name === memberName);
      if (enumInfo !== undefined) {
        return { kind: "enum", owner: cls.name, enumInfo };
      }
    }
    return undefined;
  }

  /**
   * The declared type of accessing `memberName` on `className`: a property's
   * type or a method's return type. Used by the completion/hover heuristics.
   */
  memberType(className: string, memberName: string, isCall: boolean): string | undefined {
    for (const cls of this.baseChain(className)) {
      const method = cls.methods.find((m) => m.name === memberName);
      const property = cls.properties.find((p) => p.name === memberName);
      if (isCall) {
        if (method !== undefined) {
          return method.return;
        }
      } else {
        if (property !== undefined) {
          return property.type;
        }
        if (method !== undefined) {
          return method.return;
        }
      }
    }
    return undefined;
  }

  /** All members visible on a class (walking the base chain), deduplicated by name. */
  membersOf(className: string): MemberInfo[] {
    const members: MemberInfo[] = [];
    const seen = new Set<string>();
    const push = (key: string, member: MemberInfo): void => {
      if (!seen.has(key)) {
        seen.add(key);
        members.push(member);
      }
    };
    for (const cls of this.baseChain(className)) {
      for (const method of cls.methods) {
        push(`m:${method.name}`, { kind: "method", owner: cls.name, method });
      }
      for (const property of cls.properties) {
        push(`p:${property.name}`, { kind: "property", owner: cls.name, property });
      }
      for (const signal of cls.signals) {
        push(`s:${signal.name}`, { kind: "signal", owner: cls.name, signal });
      }
      for (const constant of cls.constants) {
        push(`c:${constant.name}`, { kind: "constant", owner: cls.name, constant });
      }
      for (const enumInfo of cls.enums) {
        push(`e:${enumInfo.name}`, { kind: "enum", owner: cls.name, enumInfo });
        for (const value of enumInfo.values) {
          push(`ev:${value.name}`, {
            kind: "constant",
            owner: cls.name,
            constant: { name: value.name, value: value.value },
          });
        }
      }
    }
    return members;
  }

  /** Finds a method by name walking the base chain. */
  findMethod(className: string, methodName: string): { owner: string; method: TypeDbMethod } | undefined {
    for (const cls of this.baseChain(className)) {
      const method = cls.methods.find((m) => m.name === methodName);
      if (method !== undefined) {
        return { owner: cls.name, method };
      }
    }
    return undefined;
  }
}
