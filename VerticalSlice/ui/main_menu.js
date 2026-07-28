const menuRoot = required("menu-root");
const mainView = required("main-view");
const howView = required("how-view");
const optionsView = required("options-view");
const creditsView = required("credits-view");
const statusCopy = required("status-copy");

const playButton = required("play-button");
const howButton = required("how-button");
const optionsButton = required("options-button");
const creditsButton = required("credits-button");
const quitButton = required("quit-button");

const motionToggle = required("motion-toggle");
const motionValue = required("motion-value");
const contrastToggle = required("contrast-toggle");
const contrastValue = required("contrast-value");
let launchRequested = false;

function required(id) {
    const element = document.getElementById(id);
    if (element === null) throw new Error("Verdance menu is missing #" + id);
    return element;
}

function setHidden(element, hidden) {
    if (hidden) element.classList.add("hidden");
    else element.classList.remove("hidden");
}

function showDetail(view, focusTarget, status) {
    setHidden(mainView, true);
    setHidden(howView, view !== howView);
    setHidden(optionsView, view !== optionsView);
    setHidden(creditsView, view !== creditsView);
    statusCopy.textContent = status;
    focusTarget.focus();
}

function showMain() {
    setHidden(mainView, false);
    setHidden(howView, true);
    setHidden(optionsView, true);
    setHidden(creditsView, true);
    statusCopy.textContent = "SELECT AN OPTION";
    playButton.focus();
}

function bindBack(id) {
    required(id).addEventListener("click", showMain);
}

function bindPointerState(element) {
    element.addEventListener("mouseover", function () {
        element.classList.add("is-hovered");
    });
    element.addEventListener("mouseout", function () {
        element.classList.remove("is-hovered");
    });
    element.addEventListener("focus", function () {
        element.classList.add("is-focused");
    });
    element.addEventListener("blur", function () {
        element.classList.remove("is-focused");
    });
}

function beginJourney() {
    if (launchRequested) return;
    launchRequested = true;
    menuRoot.classList.add("launching");
    statusCopy.textContent = "OPENING THE WILDS";
    console.log("[Verdance Menu] Begin the journey");
    if (!tree.changeScene("scenes/verdance.scene")) {
        launchRequested = false;
        menuRoot.classList.remove("launching");
        statusCopy.textContent = "THE WILDS COULD NOT BE OPENED";
    }
}

// Mouse-down is bound as well as click so editor cursor capture cannot swallow
// the release frame. The guard keeps keyboard activation and normal clicks
// idempotent.
playButton.addEventListener("mousedown", beginJourney);
playButton.addEventListener("click", beginJourney);

howButton.addEventListener("click", function () {
    showDetail(howView, required("how-back"), "FIELD GUIDE");
});

optionsButton.addEventListener("click", function () {
    showDetail(optionsView, motionToggle, "PRESENTATION OPTIONS");
});

creditsButton.addEventListener("click", function () {
    showDetail(creditsView, required("credits-back"), "ARCHIVE CREDITS");
});

quitButton.addEventListener("click", function () {
    statusCopy.textContent = "LEAVING VERDANCE";
    tree.quit();
});

motionToggle.addEventListener("click", function () {
    const reduced = menuRoot.classList.toggle("motion-reduced");
    motionValue.textContent = reduced ? "REDUCED" : "DYNAMIC";
});

contrastToggle.addEventListener("click", function () {
    const soft = menuRoot.classList.toggle("soft-contrast");
    contrastValue.textContent = soft ? "SOFT" : "DEEP";
});

bindBack("how-back");
bindBack("options-back");
bindBack("credits-back");

const interactiveControls = document.querySelectorAll(".ui-hit");
for (let i = 0; i < interactiveControls.length; i += 1) {
    bindPointerState(interactiveControls[i]);
}

playButton.focus();
