# tts — runtime plugin

Text-to-speech for project scripts. Compiled into the editor, the runtime and the
Web player when `SAIDA_PLUGINS` lists `tts` (the default); active only for
projects whose `.saidaproj` contains `"plugins": ["tts"]` (SPEC §11.5).

```js
if (tts.available()) tts.speak('Collez les électrodes.', { lang: 'fr-FR', rate: 1.1, pitch: 0.8 });
tts.stop();  // cancels the current utterance and the queue
```

- Windows: SAPI, installed system voices; a voice of `lang` is preferred.
- Web: the browser's `speechSynthesis`.
- Other platforms: `tts.available()` is false and `speak` returns false.

`rate` and `pitch` are 1 for the voice's own; utterances queue in order.
