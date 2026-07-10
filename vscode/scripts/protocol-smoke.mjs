#!/usr/bin/env node
/**
 * Protocol smoke test.
 *
 * Starts a fake engine TCP server speaking the docs/PROTOCOL.md protocol
 * (hello -> welcome, request_type_db -> two type_db_class messages -> end)
 * and asserts that the LineJsonSocket + TypeDatabase modules (built to
 * dist/protocol.cjs and dist/typedb.cjs by `npm run build`) ingest it.
 *
 * Run with: npm run build && npm test
 */

import assert from "node:assert/strict";
import net from "node:net";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const distDir = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "dist");

const { LineJsonSocket, makeHello, resPathToDiskPath, diskPathToResPath } = require(
  path.join(distDir, "protocol.cjs")
);
const { TypeDatabase, normalizeTypeName, methodSignature } = require(path.join(distDir, "typedb.cjs"));

// ---------------------------------------------------------------------------
// Fake engine
// ---------------------------------------------------------------------------

const NODE_CLASS = {
  type: "type_db_class",
  name: "Node",
  base: "Object",
  native: true,
  methods: [
    {
      name: "add_child",
      return: "void",
      args: [{ name: "node", type: "Node@", default: null }],
      static: false,
      vararg: false,
    },
    { name: "get_parent", return: "Node@", args: [], static: false, vararg: false },
  ],
  properties: [{ name: "position", type: "Vector2" }],
  signals: [{ name: "ready", args: [] }],
  constants: [{ name: "NOTIFICATION_READY", value: 13 }],
  enums: [{ name: "ProcessMode", values: [{ name: "PROCESS_MODE_INHERIT", value: 0 }] }],
};

const PLAYER_CLASS = {
  type: "type_db_class",
  name: "Player",
  base: "Node",
  native: false,
  path: "res://player.as",
  line: 3,
  methods: [
    { name: "jump", return: "void", args: [{ name: "strength", type: "float", default: "1.0" }] },
  ],
  properties: [{ name: "speed", type: "float" }],
  signals: [],
  constants: [],
  enums: [],
};

function startFakeEngine() {
  return new Promise((resolve, reject) => {
    const server = net.createServer((socket) => {
      let buffer = "";
      const send = (message) => socket.write(JSON.stringify(message) + "\n");
      socket.setEncoding("utf8");
      socket.on("data", (chunk) => {
        buffer += chunk;
        let newline;
        while ((newline = buffer.indexOf("\n")) >= 0) {
          const line = buffer.slice(0, newline).trim();
          buffer = buffer.slice(newline + 1);
          if (line.length === 0) continue;
          const message = JSON.parse(line);
          if (message.type === "hello") {
            assert.equal(message.version, 1, "hello must carry protocol version 1");
            assert.ok(typeof message.client === "string" && message.client.length > 0);
            send({
              type: "welcome",
              engine: "godot",
              godot_version: "4.5.0",
              extension_version: "0.1.0",
              version: 1,
            });
          } else if (message.type === "request_type_db") {
            send({ type: "type_db_begin" });
            send(NODE_CLASS);
            send(PLAYER_CLASS);
            send({ type: "type_db_end", count: 2 });
          }
        }
      });
      socket.on("error", () => {});
    });
    server.on("error", reject);
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

async function main() {
  // Unit checks that need no socket at all.
  assert.equal(normalizeTypeName("Node@"), "Node");
  assert.equal(normalizeTypeName("const StringName &in"), "StringName");
  assert.equal(normalizeTypeName("array<int>"), "array");
  assert.equal(resPathToDiskPath("res://player.as", "/proj"), path.join("/proj", "player.as"));
  assert.equal(diskPathToResPath(path.join("/proj", "sub", "player.as"), "/proj"), "res://sub/player.as");
  assert.equal(diskPathToResPath("/elsewhere/player.as", "/proj"), undefined);

  const server = await startFakeEngine();
  const port = server.address().port;

  const db = new TypeDatabase();
  const socket = new LineJsonSocket();

  const done = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out waiting for type_db_end")), 5000);
    socket.onMessage = (message) => {
      try {
        if (message.type === "welcome") {
          assert.equal(message.version, 1);
          socket.send({ type: "request_type_db" });
        } else if (message.type === "type_db_begin") {
          db.clear();
        } else if (message.type === "type_db_class") {
          db.ingestClass(message);
        } else if (message.type === "type_db_end") {
          clearTimeout(timer);
          resolve(message);
        }
      } catch (error) {
        clearTimeout(timer);
        reject(error);
      }
    };
    socket.onError = (error) => {
      clearTimeout(timer);
      reject(error);
    };
  });

  await socket.connectAsync("127.0.0.1", port);
  socket.send(makeHello("smoke-test"));
  const end = await done;

  // Assertions on the ingested database.
  assert.equal(end.count, 2, "type_db_end must report two classes");
  assert.equal(db.count, 2, "database must contain two classes");

  const node = db.get("Node");
  assert.ok(node, "Node must be in the database");
  assert.equal(node.native, true);
  assert.equal(node.methods.length, 2);

  const player = db.get("Player");
  assert.ok(player, "Player must be in the database");
  assert.equal(player.native, false);
  assert.equal(player.base, "Node");
  assert.equal(player.path, "res://player.as");
  assert.equal(player.line, 3);

  // Base-chain walking: Player -> Node (Object is not in the DB).
  const chain = db.baseChain("Player").map((cls) => cls.name);
  assert.deepEqual(chain, ["Player", "Node"]);

  // Handle spellings resolve to the same class.
  assert.equal(db.resolve("Player@")?.name, "Player");

  // Inherited member lookup through the base chain.
  const addChild = db.findMethod("Player", "add_child");
  assert.ok(addChild, "add_child must be found on Player via Node");
  assert.equal(addChild.owner, "Node");
  assert.equal(methodSignature(addChild.method), "void add_child(Node@ node)");

  const speed = db.findMember("Player", "speed");
  assert.ok(speed && speed.kind === "property" && speed.property.type === "float");

  // memberType powers the dot-completion heuristics.
  assert.equal(db.memberType("Player", "get_parent", true), "Node@");
  assert.equal(db.memberType("Player", "position", false), "Vector2");

  // membersOf dedupes across the chain and includes enums/constants/signals.
  const memberNames = new Set(db.membersOf("Player").map((m) => m.kind));
  assert.ok(memberNames.has("method") && memberNames.has("property"));
  assert.ok(memberNames.has("signal") && memberNames.has("constant") && memberNames.has("enum"));

  socket.close();
  server.close();
  console.log("protocol-smoke: PASS (2 classes ingested, base chain and member lookups OK)");
}

main().catch((error) => {
  console.error("protocol-smoke: FAIL");
  console.error(error);
  process.exit(1);
});
