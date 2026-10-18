// node tools/verify_portable_media_web.mjs <portable-media-web-build> [Chrome]
// Exercises real browser media policy, WebAudio output and Fetch cancellation.
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const root=path.resolve(process.argv[2]);
await Promise.all(['media_web.js','media_web.wasm'].map(name=>fs.access(path.join(root,name))));
const {chromium}=await import(process.env.SAIDA_PLAYWRIGHT_MODULE?pathToFileURL(process.env.SAIDA_PLAYWRIGHT_MODULE).href:'playwright');
const frames=48000*12,tone=Buffer.alloc(44+frames*4);
tone.write('RIFF');tone.writeUInt32LE(tone.length-8,4);tone.write('WAVEfmt ',8);tone.writeUInt32LE(16,16);
tone.writeUInt16LE(1,20);tone.writeUInt16LE(2,22);tone.writeUInt32LE(48000,24);tone.writeUInt32LE(192000,28);
tone.writeUInt16LE(4,32);tone.writeUInt16LE(16,34);tone.write('data',36);tone.writeUInt32LE(frames*4,40);
for(let i=0;i<frames;++i){const sample=Math.round(Math.sin(i*440*2*Math.PI/48000)*9000);tone.writeInt16LE(sample,44+i*4);tone.writeInt16LE(sample,46+i*4);}
const streams=new Set();
const media=http.createServer((req,res)=>{
    streams.add(res);res.on('close',()=>streams.delete(res));
    if(req.url!='/no-cors.wav')res.setHeader('Access-Control-Allow-Origin','*');
    if(req.url==='/slow'){setTimeout(()=>{if(!res.destroyed)res.end('late');},600);return;}
    if(req.url==='/json'){res.setHeader('Content-Type','application/json');res.end('{"station":"cœur"}');return;}
    if(req.url==='/large'){res.end('x'.repeat(10000));return;}
    if(req.url==='/404'){res.writeHead(404);res.end('missing');return;}
    res.setHeader('Content-Type','audio/wav');res.end(tone);
});
const server=http.createServer(async(req,res)=>{
    const name=path.basename(new URL(req.url,'http://localhost').pathname);
    if(!name){res.setHeader('Content-Type','text/html');res.end('<!doctype html><button id="start">Start audio</button><script>var Module={onRuntimeInitialized(){window.ready=true;}};</script><script src="media_web.js"></script>');return;}
    try{res.setHeader('Content-Type',name.endsWith('.wasm')?'application/wasm':'text/javascript');res.end(await fs.readFile(path.join(root,name)));}
    catch{res.writeHead(404);res.end();}
});
await Promise.all([new Promise(r=>media.listen(0,'127.0.0.1',r)),new Promise(r=>server.listen(0,'127.0.0.1',r))]);
const base=`http://127.0.0.1:${media.address().port}`;
let browser;
try{
    browser=await chromium.launch({headless:true,executablePath:process.argv[3],args:['--autoplay-policy=document-user-activation-required']});
    const page=await browser.newPage();
    const errors=[];page.on('pageerror',e=>{errors.push(e.message);console.error('Browser error:',e.message);});
    const session=await page.context().newCDPSession(page);
    // Playwright's normal evaluate grants user activation. CDP with that flag
    // disabled is required to genuinely exercise a browser's autoplay block.
    const evaluate=async expression=>{
        const value=await session.send('Runtime.evaluate',{expression,userGesture:false,returnByValue:true});
        if(value.exceptionDetails)throw Error(value.exceptionDetails.text);return value.result.value;
    };
    const wait=async expression=>{const start=Date.now();while(!await evaluate(expression)){if(Date.now()-start>10000)throw Error('Timed out: '+expression);await new Promise(r=>setTimeout(r,50));}};
    await page.goto(`http://127.0.0.1:${server.address().port}/`);await wait('window.ready');
    const call=(name,types=[],values=[],result='number')=>evaluate(`Module.ccall(${JSON.stringify(name)},${JSON.stringify(result)},${JSON.stringify(types)},${JSON.stringify(values)})`);
    assert.equal(await call('media_open',['string'],['file:///private.wav']),0);
    assert.equal(await call('media_state'),5);
    await call('media_open',['string'],[base+'/tone.wav']);
    await wait("Module.ccall('media_state','number',[],[])===6");
    await page.click('#start');
    await page.waitForFunction(()=>Module.ccall('media_state','number',[],[])===2);
    await page.evaluate(()=>{const service=Module.saidaAudio,job=[...service.jobs.values()][0];job.analyser=service.context.createAnalyser();job.gain.connect(job.analyser);});
    const rms=()=>page.evaluate(()=>{const job=[...Module.saidaAudio.jobs.values()][0],buffer=new Float32Array(job.analyser.fftSize);job.analyser.getFloatTimeDomainData(buffer);return Math.sqrt(buffer.reduce((a,x)=>a+x*x,0)/buffer.length);});
    const waitRms=async audible=>{const end=Date.now()+5000;while(Date.now()<end){const level=await rms();if(audible?level>.01:level<.00001)return;await page.waitForTimeout(50);}throw Error(audible?'decoded audio does not reach the WebAudio graph':'volume gain does not mute decoded audio');};
    await waitRms(true);
    await call('media_volume',['number'],[0]);await waitRms(false);
    await call('media_volume',['number'],[.5]);await waitRms(true);
    await call('media_stop');assert.equal(await call('media_state'),0);
    await call('media_open',['string'],[base+'/tone.wav']);await call('media_open',['string'],[base+'/tone.wav?replacement']);
    await page.waitForFunction(()=>Module.ccall('media_state','number',[],[])===2);
    assert.equal(await page.evaluate(()=>Module.saidaAudio.jobs.size),1,'station replacement releases the previous element');
    await call('media_stop');await page.waitForTimeout(100);assert.equal(await page.evaluate(()=>Module.saidaAudio.jobs.size),0);
    await call('media_open',['string'],[base+'/no-cors.wav']);await page.waitForFunction(()=>Module.ccall('media_state','number',[],[])===5);await call('media_stop');
    console.log('PASS browser audio: gesture, decoded PCM, gain, stop, replacement, CORS failure');
    async function get(url,timeout=2,budget=16384){await call('http_get',['string','number','number'],[url,timeout,budget]);await page.waitForFunction(()=>Module.ccall('http_poll','number',[],[])===1);return {code:await call('http_code'),body:await call('http_body',[],[],'string'),error:await call('http_failure',[],[],'string')};}
    const json=await get(base+'/json');assert.equal(json.code,200);assert.deepEqual(JSON.parse(json.body),{station:'cœur'});assert.equal(json.error,'');
    assert.match((await get(base+'/large',2,128)).error,/budget/);
    assert.match((await get(base+'/slow',.05)).error,/timeout/);
    assert.equal((await get(base+'/404')).code,404);
    assert.ok((await get(base+'/no-cors.wav')).error);
    assert.ok((await get('file:///private')).error);
    await call('http_get',['string','number','number'],[base+'/slow',2,100]);await call('http_abort');await page.waitForTimeout(650);assert.equal(await call('http_poll'),0);
    assert.deepEqual(errors,[]);
    console.log('PASS browser HTTP: JSON UTF-8, size budget, timeout, status, CORS, URL validation, cancellation');
    if(process.argv[4]==='--live'){
        const catalogue=await get('https://all.api.radio-browser.info/json/servers',6,1024*1024);
        assert.equal(catalogue.code,200);assert.ok(JSON.parse(catalogue.body).length>0);
        await call('media_open',['string'],['https://icecast.radiofrance.fr/fip-midfi.mp3']);
        await page.waitForFunction(()=>[2,5].includes(Module.ccall('media_state','number',[],[])),{},{timeout:15000});
        assert.equal(await call('media_state'),2,'real MP3 station plays in the browser');
        await page.evaluate(()=>{const service=Module.saidaAudio,job=[...service.jobs.values()][0];job.analyser=service.context.createAnalyser();job.gain.connect(job.analyser);});
        await waitRms(true);await call('media_stop');
        console.log('PASS live browser catalogue and decoded Radio France MP3');
    }
}finally{
    await browser?.close();for(const response of streams)response.destroy();
    await Promise.all([new Promise(r=>server.close(r)),new Promise(r=>media.close(r))]);
}
