// Validate the actual translated water pipelines in a WebGPU browser.
// node tools/verify_water_web.mjs <directory-of-WGSL> [browser-executable]
// Playwright may be supplied through SAIDA_PLAYWRIGHT_MODULE.
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const root = path.resolve(process.argv[2]);
const modulePath = process.env.SAIDA_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(modulePath).href : 'playwright');
const server = http.createServer(async (request, response) => {
    const name = path.basename(new URL(request.url, 'http://localhost').pathname);
    if (!name) { response.end('<!doctype html><title>Water WebGPU verification</title>'); return; }
    try { response.end(await fs.readFile(path.join(root, name))); }
    catch { response.writeHead(404); response.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
    browser = await chromium.launch({headless:true, executablePath:process.argv[3],
        args:['--enable-unsafe-webgpu']});
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const result = await page.evaluate(async () => {
        const adapter = await navigator.gpu?.requestAdapter();
        if (!adapter) throw new Error('No WebGPU adapter');
        const device = await adapter.requestDevice();
        const errors = [];
        device.addEventListener('uncapturederror', event => errors.push(event.error.message));
        const modules = {};
        for (const name of ['water.vert','water_surface.vert','water.frag',
                            'cartoon_water.vert','cartoon_water_surface.vert','cartoon_water.frag']) {
            const response = await fetch(name+'.wgsl');
            if (!response.ok) throw new Error('Missing shader '+name);
            const module = device.createShaderModule({label:name,code:await response.text()});
            const info = await module.getCompilationInfo();
            for (const m of info.messages) if (m.type === 'error') errors.push(name+': '+m.message);
            modules[name] = module;
        }
        if (errors.length) throw new Error(errors.join('\n'));
        for (const prefix of ['water','cartoon_water']) {
            for (const surface of [false,true]) {
                await device.createRenderPipelineAsync({label:prefix+(surface?' surface':' grid'),layout:'auto',
                    vertex:{module:modules[prefix+(surface?'_surface':'')+'.vert'],entryPoint:'main',
                        buffers:surface ? [{arrayStride:12,attributes:[{shaderLocation:0,offset:0,format:'float32x3'}]}] : []},
                    fragment:{module:modules[prefix+'.frag'],entryPoint:'main',targets:[{format:'rgba16float'}]},
                    primitive:{topology:'triangle-list',cullMode:'none'},
                    depthStencil:{format:'depth32float',depthWriteEnabled:true,depthCompare:'greater'}});
            }
        }
        await device.queue.onSubmittedWorkDone();
        if (errors.length) throw new Error(errors.join('\n'));
        const result = {pipelines:4,adapter:{vendor:adapter.info.vendor,architecture:adapter.info.architecture},errors};
        device.destroy();
        return result;
    });
    await fs.writeFile(path.join(root,'browser-validation.json'),JSON.stringify(result,null,2));
    console.log('PASS WebGPU water pipelines:',JSON.stringify(result));
} finally {
    await browser?.close();
    await new Promise(resolve => server.close(resolve));
}
