const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const script = fs.readFileSync(path.join(__dirname,
    '../packaging/windows/ifw/maintenance_installscript.qs'), 'utf8');

function load(isInstaller, requested) {
    const selected = [];
    let installationStarted;
    const scope = {
        installer: {
            isInstaller: () => isInstaller,
            setInstallerBaseBinary: value => selected.push(value),
            installationStarted: {
                connect: (owner, callback) => { installationStarted = callback.bind(owner); },
            },
        },
        component: {
            updateRequested: () => requested,
            installationRequested: () => requested,
        },
    };
    vm.createContext(scope);
    vm.runInContext(script, scope);
    vm.runInContext('new Component()', scope);
    return { selected, start: () => installationStarted() };
}

// IFW computes and caches installationResourcesSpace before installationStarted.
// At that boundary, its embedded base is added only if this path is empty.
const install = load(true, true);
assert.deepEqual(install.selected, ['@TargetDir@/installerbase.exe']);
install.start();
assert.equal(install.selected.at(-1), '@TargetDir@/installerbase.exe');

const uninstall = load(false, false);
assert.deepEqual(uninstall.selected, []);
uninstall.start();
assert.deepEqual(uninstall.selected, []);

const update = load(false, true);
assert.deepEqual(update.selected, []);
update.start();
assert.deepEqual(update.selected, ['@TargetDir@/installerbase.exe']);
console.log('Windows IFW maintenance lifecycle passed.');
