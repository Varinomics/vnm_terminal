const fs = require('fs');
const vm = require('vm');
const assert = require('assert/strict');
const path = require('path');
const childProcess = require('child_process');
const source = fs.readFileSync(path.join(__dirname,'../packaging/windows/ifw/controller.qs'),'utf8');
const support = fs.readFileSync(path.join(__dirname,'../packaging/windows/ifw/replacement_support.ps1'),'utf8');
const original = 'C:\\Program Files\\vnm_terminal';
const status = {Success:0, Failure:1, Running:2, Canceled:3, Unfinished:4};
function fixture(options = {}) {
    const steps = [];
    const inputs = [];
    const callbacks = {};
    let exists = options.exists !== false;
    let change;
    const field = { text:original,
        textChanged:{connect:f => {change = f;}},
        setText:t => {field.text=t; if(change) change(t);}
    };
    const message = {text:'framework detail',setText(t){this.text=t;}};
    const page = {TargetDirectoryLineEdit:field, MessageLabel:message,
        RunItCheckBox:{hide(){steps.push('hide launch');}},subTitle:''};
    const context = {
        installer:{
            isInstaller:() => true,
            isOfflineGenerator:() => false,
            isPackageViewer:() => false,
            isCommandLineInstance:() => false,
            isProcessRunning:() => false,
            setDefaultPageVisible:() => {},
            aboutCalculateComponentsToInstall:{connect:(owner,callback) => {
                callbacks.install = () => callback.call(owner);
            }},
            aboutCalculateComponentsToUninstall:{connect:(owner,callback) => {
                callbacks.uninstall = () => callback.call(owner);
            }},
            value:k => k === 'RootDir' ? path.parse(process.env.SystemRoot).root : original,
            toNativeSeparators:p => p,
            fileExists:p => (p.endsWith('powershell.exe') && !options.missingHost)
                || (p.endsWith('vnm_terminal_maintenance.exe') && exists),
            hasAdminRights:() => options.admin === true,
            readFile:p => p.endsWith('log_path_probe.ps1') ? '' : (options.missingLibrary ? '' : support),
            setValue:(k,v) => {if(k !== 'LogFileName') steps.push([k,v]);},
            execute:(p,a,input) => {
                inputs.push({program:p,args:a,input:input});
                const action=input.includes("-Action 'Validate'") ? 'Validate' : 'Remove';
                steps.push(action);
                if(action==='Validate') return options.validation || ['validated',0];
                if(options.removal) return options.removal;
                exists=false;
                return ['removed',0];
            },
            status:options.status || 0
        },
        QMessageBox:{Yes:1,No:2,question:(id,title,detail,buttons,defaultButton) => {
            assert.equal(defaultButton,2,'Keeping the existing installation must be the default answer');
            assert.equal(buttons,3,'Consent must offer Yes and No');
            assert.match(detail,/cannot be undone/);
            assert.match(detail,/settings outside the installation folder are preserved/);
            steps.push('confirm');
            return options.no ? 2 : 1;
        },
            critical:(id,title,detail) => steps.push(['error',id,detail])},
        QInstaller:{...status,ComponentSelection:5},
        gui:{pageWidgetByObjectName:() => page,rejectWithoutPrompt:() => steps.push('close'),
            findChild:() => ({hideColumn:index => assert.equal(index,5)})}
    };
    vm.createContext(context);
    vm.runInContext(source,context);
    const wizard = new context.Controller();
    return {context,steps,inputs,page,callbacks,wizard,controller:context.Controller.prototype};
}
let f=fixture({exists:false});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.length,0,'Fresh folder must not prompt or remove');
f=fixture({no:true});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.join(','),'Validate,confirm');
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.join(','),'Validate,confirm','Re-entry must preserve declined answer');
f.page.TargetDirectoryLineEdit.setText(original + '2');
assert.equal(f.steps.join(','),'Validate,confirm','Typing must not launch a replacement probe');
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.join(','),'Validate,confirm,Validate,confirm',
    'Entering the page with a changed folder must allow a separate answer');
f=fixture({admin:true});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.length,1);
assert.equal(f.steps[0][0],'error','Elevated setup must refuse before running a helper');
f=fixture({validation:['unsigned maintenance',1]});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps[0],'Validate');
assert.equal(f.steps.length,2,'Invalid image must not reach removal consent');
for (const options of [{missingHost:true},{missingLibrary:true}]) {
    f=fixture(options);
    f.controller.TargetDirectoryPageCallback();
    assert.equal(f.inputs.length,0,'Missing helper material must not execute a command');
    assert.equal(f.steps.length,1,'Missing helper material must stop before consent');
    assert.equal(f.steps[0][0],'error');
}
for (const refusal of ['Permission request canceled', 'Running session found during revalidation']) {
    f=fixture({removal:['not-started\r\n' + refusal,2]});
    f.controller.TargetDirectoryPageCallback();
    assert.equal(f.controller.removedInstallationDirectory,'');
    assert.equal(f.controller.replacementStopped,false);
    assert.equal(f.controller.promptedExistingInstallationDirectory,'');
    assert.equal(f.steps.length,4,'A proven pre-launch refusal must leave setup open');
    assert.match(f.steps[3][2],/previous installation is unchanged/);
    f.controller.TargetDirectoryPageCallback();
    assert.equal(f.steps.length,8,'Returning to the page must allow a corrected retry');
}
f=fixture({removal:['Uncertain launch failure',1]});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.controller.removedInstallationDirectory,'','Failure must not claim removal completed');
assert.equal(f.steps[3][0],'error');
assert.match(f.steps[3][2],/previous installation may have changed/);
assert.equal(f.steps[4],'close','Uncertain removal must close setup before any replacement');
const failureSteps=f.steps.length;
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.length,failureSteps+1,'Failure must repeat the close request');
assert.equal(f.steps.at(-1),'close','Failure must not allow another removal or continuation');
f=fixture({removal:['',2]});
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps[4],'close','An exit code alone must not prove that removal never started');
for (const result of [['not-started\r\nAmbiguous outcome',1],['removed',1],['unexpected',0],[]]) {
    f=fixture({removal:result});
    f.controller.TargetDirectoryPageCallback();
    assert.equal(f.controller.removedInstallationDirectory,'');
    assert.equal(f.controller.replacementStopped,true);
    assert.equal(f.steps[4],'close','Only observed successful removal may continue setup');
}
f=fixture();
const originalWizard=f.wizard;
f.controller.TargetDirectoryPageCallback();
assert.deepEqual(f.steps.slice(0,3),['Validate','confirm','Remove']);
assert.equal(f.controller.removedInstallationDirectory,original);
assert.match(f.page.MessageLabel.text,/Continue with this setup/);
assert.deepEqual(f.steps[3],['TargetDir',original],'Replacement must preserve the selected installation target');
assert.equal(f.wizard,originalWizard,'Continuation uses the existing wizard');
const completedSteps=f.steps.length;
f.controller.TargetDirectoryPageCallback();
assert.equal(f.steps.length,completedSteps,'Returning after removal must not launch another remover');
assert.equal(f.callbacks.install(),undefined,'The existing installation callback must remain connected');
assert.equal(f.callbacks.uninstall(),undefined,'The existing removal callback must remain connected');
f.controller.FinishedPageCallback();
assert.match(f.page.MessageLabel.text,/previous installation was removed/);
f=fixture({status:status.Canceled});
f.controller.removedInstallationDirectory=original;
f.controller.FinishedPageCallback();
assert.match(f.page.MessageLabel.text,/Installation was canceled/);
assert.match(f.page.MessageLabel.text,/previous installation was removed/);
assert.equal(f.steps[0],'hide launch');
f=fixture({status:status.Failure});
f.controller.removedInstallationDirectory=original;
f.controller.FinishedPageCallback();
assert.match(f.page.MessageLabel.text,/Installation failed/);
assert.match(f.page.MessageLabel.text,/User settings outside that folder were preserved/);
assert.equal(f.steps[0],'hide launch');
f=fixture({status:status.Unfinished});
f.controller.removedInstallationDirectory=original;
f.controller.FinishedPageCallback();
assert.match(f.page.MessageLabel.text,/Installation did not complete/);
assert.match(f.page.MessageLabel.text,/previous installation was removed/);
assert.equal(f.steps[0],'hide launch');
for (const [target,action] of [
    ['C:\\vnm_terminal-replacement-unsupported-target','Validate'],
    ['C:\\vnm_terminal-replacement-unsupported-target','Remove'],
    ["C:\\vnm_terminal-replacement-unsupported'; Write-Output 'unsafe-target'; #",'Validate']
]) {
    f=fixture();
    f.controller.runReplacementHelper(target,action);
    const request=f.inputs[0];
    let rejected;
    try {
        childProcess.execFileSync(path.join(process.env.SystemRoot,
            'System32/WindowsPowerShell/v1.0/powershell.exe'),request.args,
            {input:request.input,encoding:'utf8',windowsHide:true,timeout:60000});
    } catch(error) {
        rejected=error;
    }
    assert.ok(rejected,'Exact stdin composition must execute the unsupported-target refusal');
    assert.equal(rejected.status,2,'The native host must report refusal before removal starts; '
        + 'error=' + rejected.code + ', signal=' + rejected.signal);
    assert.match(rejected.stdout,/protected vnm_terminal installation folder only|administrator rights/);
    assert.match(rejected.stdout,/^not-started\r?\n/);
    assert.doesNotMatch(rejected.stdout,/unsafe-target/,'Quoted target text must remain an argument');
    assert.equal(rejected.stderr,'');
}
console.log('Controller source behavior and exact Windows PowerShell stdin fixtures passed. '
    + 'No wizard, elevation, removal or lifecycle coverage.');
