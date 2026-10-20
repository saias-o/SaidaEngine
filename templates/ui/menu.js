const places = document.querySelectorAll('.ui-list-item');
let destination = 'The quiet coast';
for (const place of places) {
    place.addEventListener('click', () => {
        for (const other of places) other.classList.remove('selected');
        place.classList.add('selected');
        destination = place.textContent;
        document.getElementById('selection').textContent = destination;
    });
}
document.getElementById('settings').addEventListener('click', () => {
    const panel = document.getElementById('settings-panel');
    panel.hidden = !panel.hidden;
    if (!panel.hidden) document.getElementById('traveller').focus();
});
document.getElementById('explore').addEventListener('click', () => {
    const name = document.getElementById('traveller').value;
    document.getElementById('selection').textContent = name + ', your next horizon: ' + destination;
});
