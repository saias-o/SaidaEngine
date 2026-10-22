// Validate bloom against the actual WebGPU layout, and reject the former vertex
// shader which accidentally required tonemap's depth binding and push block.
// node tools/verify_postprocess_web.mjs <WGSL-directory> [browser-executable]
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const root = path.resolve(process.argv[2]);
const modulePath = process.env.SAIDA_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(modulePath).href : 'playwright');
const server = http.createServer(async (request, response) => {
    const name = path.basename(new URL(request.url, 'http://localhost').pathname);
    if (!name) { response.end('<!doctype html><title>Bloom pipeline verification</title>'); return; }
    try { response.end(await fs.readFile(path.join(root, name))); }
    catch { response.writeHead(404); response.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
    browser = await chromium.launch({ headless: true, executablePath: process.argv[3],
        args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--use-angle=d3d11'] });
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const result = await page.evaluate(async () => {
        const adapter = await navigator.gpu?.requestAdapter();
        if (!adapter) throw new Error('No WebGPU adapter');
        const device = await adapter.requestDevice();
        const errors = [];
        device.addEventListener('uncapturederror', event => errors.push(event.error.message));
        const modules = {};
        for (const name of ['fullscreen.vert', 'tonemap.vert', 'bloom_downsample.frag', 'bloom_upsample.frag']) {
            const response = await fetch(`${name}.wgsl`);
            if (!response.ok) throw new Error(`Missing ${name}.wgsl`);
            const module = device.createShaderModule({ label: name, code: await response.text() });
            const info = await module.getCompilationInfo();
            for (const message of info.messages)
                if (message.type === 'error') throw new Error(`${name}: ${message.message}`);
            modules[name] = module;
        }
        const input = device.createBindGroupLayout({ entries: [
            { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } }] });
        const empty = device.createBindGroupLayout({ entries: [] });
        const push = device.createBindGroupLayout({ entries: [
            { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
              buffer: { type: 'uniform', minBindingSize: 32 } }] });
        const layout = device.createPipelineLayout({ bindGroupLayouts: [input, empty, empty, push] });
        const pipeline = (vertex, fragment) => device.createRenderPipelineAsync({ layout,
            vertex: { module: modules[vertex], entryPoint: 'main' },
            fragment: { module: modules[fragment], entryPoint: 'main', targets: [{ format: 'rgba16float' }] },
            primitive: { topology: 'triangle-list', cullMode: 'none' } });
        for (const fragment of ['bloom_downsample.frag', 'bloom_upsample.frag'])
            await pipeline('fullscreen.vert', fragment);
        let rejected = false;
        try { await pipeline('tonemap.vert', 'bloom_downsample.frag'); }
        catch { rejected = true; }
        if (!rejected) throw new Error('The old tonemap vertex/layout mismatch escaped the check');
        await device.queue.onSubmittedWorkDone();
        if (errors.length) throw new Error(errors.join('\n'));
        device.destroy();
        return { pipelines: 2, legacyMismatchRejected: rejected };
    });
    console.log('PASS WebGPU bloom pipeline layouts:', JSON.stringify(result));
} finally {
    await browser?.close();
    await new Promise(resolve => server.close(resolve));
}
