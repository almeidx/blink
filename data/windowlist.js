// KWin script run for each region selection. Reports the windows on screen, topmost first, as
// [x, y, width, height] in logical compositor coordinates, so the selection can snap to them.
// Blink prepends the definitions of blinkService and blinkRequest.
const windows = [];
const desktop = workspace.currentDesktop.id;
const activity = workspace.currentActivity;
const order = workspace.stackingOrder;
for (let i = order.length - 1; i >= 0; --i) {
    const w = order[i];
    if (w.minimized || w.hidden || w.deleted)
        continue;
    if (!w.onAllDesktops && !w.desktops.some(d => d.id === desktop))
        continue;
    if (w.activities.length > 0 && !w.activities.includes(activity))
        continue;
    const g = w.frameGeometry;
    if (g.width > 0 && g.height > 0)
        windows.push([g.x, g.y, g.width, g.height]);
}
callDBus(blinkService, "/WindowList", "io.github.almeidx.Blink.WindowList", "report", blinkRequest, JSON.stringify(windows));
