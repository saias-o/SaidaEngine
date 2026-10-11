// Compile the actual translated scene pipeline, including billboard varyings.
// node tools/verify_billboard_web.mjs <WGSL-directory> [browser-executable]
// SAIDA_PLAYWRIGHT_MODULE can point to the installed Playwright entry point.
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const root = path.resolve(process.argv[2]);
const modulePath = process.env.SAIDA_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(modulePath).href : 'playwright');
const server = http.createServer(async (request, response) => {
    const name = path.basename(new URL(request.url, 'http://localhost').pathname);
    if (!name) { response.end('<!doctype html><title>Billboard WebGPU verification</title>'); return; }
    try { response.end(await fs.readFile(path.join(root, name))); }
    catch { response.writeHead(404); response.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
    browser = await chromium.launch({headless:true, executablePath:process.argv[3],
        args:['--enable-unsafe-webgpu','--ignore-gpu-blocklist','--use-angle=d3d11']});
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const result = await page.evaluate(async () => {
        const adapter = await navigator.gpu?.requestAdapter();
        if (!adapter) throw new Error('No WebGPU adapter');
        const device = await adapter.requestDevice();
        const errors = [];
        device.addEventListener('uncapturederror', event => errors.push(event.error.message));
        const modules = {};
        for (const stage of ['vert','frag']) {
            const response = await fetch(`shader.${stage}.wgsl`);
            if (!response.ok) throw new Error(`Missing shader ${stage}`);
            const module = device.createShaderModule({label:stage,code:await response.text()});
            const info = await module.getCompilationInfo();
            for (const message of info.messages)
                if (message.type === 'error') errors.push(`${stage}: ${message.message}`);
            modules[stage] = module;
        }
        if (errors.length) throw new Error(errors.join('\n'));
        // Mirror rhi/webgpu/Pipeline.cpp's shared 100-byte Vertex layout.
        const attributes = ['float32x3','float32x3','float32x3','float32x2',
                            'float32x2','float32x4','sint32x4','float32x4']
            .map((format, shaderLocation) => ({format, shaderLocation,
                offset:[0,12,24,36,44,52,68,84][shaderLocation]}));
        await device.createRenderPipelineAsync({label:'Relightable billboard scene',layout:'auto',
            vertex:{module:modules.vert,entryPoint:'main',buffers:[{arrayStride:100,attributes}]},
            fragment:{module:modules.frag,entryPoint:'main',targets:[{format:'rgba16float'}]},
            primitive:{topology:'triangle-list',cullMode:'none'},
            depthStencil:{format:'depth32float',depthWriteEnabled:true,depthCompare:'greater'}});
        await device.queue.onSubmittedWorkDone();
        if (errors.length) throw new Error(errors.join('\n'));
        const result = {pipelines:1,adapter:{vendor:adapter.info.vendor,architecture:adapter.info.architecture},errors};
        device.destroy();
        return result;
    });
    console.log('PASS WebGPU relightable billboard pipeline:',JSON.stringify(result));
} finally {
    await browser?.close();
    await new Promise(resolve => server.close(resolve));
}
