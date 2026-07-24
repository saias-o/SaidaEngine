// A syntactically valid gameplay-style script (compile-check only, not run).
function onReady() {
  this.speed = 2.0;
}

function onUpdate(dt) {
  const delta = dt * this.speed;
  return delta;
}
