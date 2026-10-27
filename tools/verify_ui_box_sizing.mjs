// Compare the same unmodified HTML/CSS fixture in Saida and Chromium.
// node tools/verify_ui_box_sizing.mjs [build-directory] [browser-executable]
// SAIDA_PLAYWRIGHT_MODULE may point to an installed Playwright entry point.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const build = path.resolve(process.argv[2] ?? path.join(root, 'build-rel'));
const fixture = path.join(root, 'tests/fixtures/ui-project');
const output = path.join(build, 'ui-box-sizing-browser');
await fs.mkdir(output, { recursive: true });
async function renderNative(name) {
    const jsonPath = path.join(output, `${name}-saida.json`);
    const result = spawnSync(path.join(build, 'bin', process.platform === 'win32' ? 'saida_tool.exe' : 'saida_tool'),
        ['render-ui', `ui/${name}.html`, '--project', fixture, '--size', '800x1800',
         '--out', path.join(output, `${name}-saida.png`), '--layout-json', jsonPath], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr || String(result.error));
    const native = JSON.parse(await fs.readFile(jsonPath, 'utf8'));
    assert.equal(native.diagnosticCount, 0, JSON.stringify(native.diagnostics));
    assert.equal(native.elements.tag, 'html');
    const boxes = {};
    function collect(element) {
        if (element.id) boxes[element.id] = element.borderBox;
        for (const child of element.children ?? []) collect(child);
    }
    collect(native.elements);
    return boxes;
}
const modulePath = process.env.SAIDA_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(modulePath).href : 'playwright');
const server = http.createServer(async (request, response) => {
    const name = new URL(request.url, 'http://localhost').pathname;
    if (!['/box-sizing.html', '/box-sizing.css', '/box-sizing-reset.html'].includes(name)) {
        response.writeHead(404); response.end(); return;
    }
    try {
        response.setHeader('Content-Type', name.endsWith('.css') ? 'text/css' : 'text/html');
        response.end(await fs.readFile(path.join(fixture, 'ui', name.slice(1))));
    } catch {
        response.writeHead(500); response.end();
    }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
    browser = await chromium.launch({ headless: true, executablePath: process.argv[3] });
    const page = await browser.newPage({ viewport: { width: 800, height: 1800 } });
    const cases = [
        ['box-sizing', ['content', 'border', 'inherited', 'barrier-child', 'initial', 'unset', 'variable',
                 'percentage', 'flex-a', 'flex-b', 'minimum', 'maximum', 'tiny', 'button',
                 'select', 'input-text', 'checkbox', 'textarea']],
        ['box-sizing-reset', ['real-body', 'reset-div', 'reset-button', 'reset-checkbox', 'escape', 'root-variable']],
    ];
    let count = 0;
    for (const [name, ids] of cases) {
        const nativeBoxes = await renderNative(name);
        await page.goto(`http://127.0.0.1:${server.address().port}/${name}.html`);
        const boxes = await page.evaluate(ids => Object.fromEntries(ids.map(id => {
            const element = document.getElementById(id);
            const { width, height } = element.getBoundingClientRect();
            return [id, { width, height, boxSizing: getComputedStyle(element).boxSizing }];
        })), ids);
        await fs.writeFile(path.join(output, `${name}-browser.json`), JSON.stringify(boxes, null, 2));
        for (const id of ids)
            for (const axis of ['width', 'height'])
                assert.ok(Math.abs(boxes[id][axis] - nativeBoxes[id][axis]) < 0.01,
                    `${name}/${id}.${axis}: browser=${boxes[id][axis]}, Saida=${nativeBoxes[id][axis]}`);
        count += ids.length;
    }
    console.log(`[ui-box-sizing-browser] PASS: ${count} unmodified HTML/CSS boxes match Chromium, including the html/:root reset`);
} finally {
    await browser?.close();
    await new Promise(resolve => server.close(resolve));
}
