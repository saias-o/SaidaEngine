#!/usr/bin/env node
// stdio <-> TCP bridge for the SaidaEngine in-process MCP server.
//
// Claude Desktop launches MCP servers and talks to them over stdio (one
// newline-delimited JSON-RPC message per line). The engine's McpServer speaks
// the *same* newline-delimited JSON-RPC, but over a localhost TCP socket
// (default 127.0.0.1:8765, see EditorUI.cpp / SAIDA_MCP_PORT). Both ends use
// identical framing, so this process is just a byte pump between them.
//
// Launch order does not matter: stdin is buffered until the engine socket is
// up, so you can start Claude Desktop first and open the SaidaEngine editor
// afterwards (within Claude's initialize timeout, ~1 min). When the editor
// quits, the engine socket closes and this bridge exits; relaunch the editor
// and reconnect the server in Claude Desktop.
//
// Usage (configured in claude_desktop_config.json):
//   node mcp-stdio-bridge.mjs [port]
// Port resolution: SAIDA_MCP_PORT env > argv[2] > 8765.

import net from "node:net";

const HOST = "127.0.0.1";
const PORT = Number(process.env.SAIDA_MCP_PORT || process.argv[2] || 8765);
const RETRY_MS = 500;

const log = (...a) =>
  process.stderr.write(`[saida-bridge] ${a.join(" ")}\n`);

let socket = null;
let connected = false;
const pending = []; // stdin chunks buffered until the engine socket is up

// Claude Desktop -> engine (buffer until connected so launch order is free).
process.stdin.on("data", (chunk) => {
  if (connected) socket.write(chunk);
  else pending.push(chunk);
});
process.stdin.on("end", () => {
  if (connected) socket.end();
});

function connect() {
  const s = net.createConnection({ host: HOST, port: PORT });

  s.on("connect", () => {
    socket = s;
    connected = true;
    log(`connected to SaidaEngine MCP on ${HOST}:${PORT}`);
    for (const chunk of pending) s.write(chunk);
    pending.length = 0;
    s.pipe(process.stdout); // engine -> Claude Desktop
  });

  s.on("error", (err) => {
    if (!connected) {
      // Editor not up yet (or wrong port): keep retrying quietly.
      setTimeout(connect, RETRY_MS);
    } else {
      log(`socket error: ${err.message}`);
      process.exit(1);
    }
  });

  s.on("close", () => {
    if (connected) {
      log("engine disconnected (editor closed?); exiting");
      process.exit(0);
    }
  });
}

log(`waiting for SaidaEngine MCP on ${HOST}:${PORT} — open the editor…`);
connect();
