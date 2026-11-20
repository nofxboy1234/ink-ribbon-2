import type { Builder, Configurer, Project } from 'jsr:@floooh/fibs@^1';

/* Local sokol and dcimgui submodules keep the build self-contained; the
   toolchain, configs and build graph come from fibs. */
export function configure(c: Configurer): void {
    c.addCommand({
        name: 'test',
        help: () => console.log('build and run the core tests'),
        run: runTests,
    });
    c.addCommand({
        name: 'verify',
        help: () => console.log('build and run the full editor verification suite'),
        run: runVerify,
    });
    c.addCommand({
        name: 'editor',
        help: () => console.log('run the editor from the project directory'),
        run: runEditor,
    });
}

async function runProcess(cmd: string, args: string[], cwd: string): Promise<number> {
    const proc = new Deno.Command(cmd, {
        args,
        cwd,
        stdout: 'inherit',
        stderr: 'inherit',
    });
    const result = await proc.output();
    return result.code;
}

async function runTests(project: Project): Promise<void> {
    const root = project.dir();
    const fibs = `${root}/fibs`;
    let code = await runProcess(fibs, ['build', 'test_level'], root);
    if (code !== 0) {
        Deno.exitCode = code;
        return;
    }
    const config = project.activeConfig();
    const bin = `${project.targetDistDir('test_level', config.name)}/test_level`;
    code = await runProcess(bin, [], root);
    Deno.exitCode = code;
}

async function runEditor(project: Project, args: string[]): Promise<void> {
    const root = project.dir();
    let code = await runProcess(`${root}/fibs`, ['build', 'editor'], root);
    if (code !== 0) {
        Deno.exitCode = code;
        return;
    }
    const config = project.activeConfig();
    const bin = `${project.targetDistDir('editor', config.name)}/editor`;
    code = await runProcess(bin, args.slice(1), root);
    Deno.exitCode = code;
}

async function runVerify(project: Project): Promise<void> {
    const root = project.dir();
    const code = await runProcess(`${root}/tools/verify.sh`, [], root);
    Deno.exitCode = code;
}

export function build(b: Builder): void {
    b.setProjectName('ink-ribbon-2');
    b.addCmakeVariable('CMAKE_C_STANDARD', '11');
    b.addCmakeVariable('CMAKE_C_STANDARD_REQUIRED', 'ON');
    b.addCmakeVariable('CMAKE_C_EXTENSIONS', 'ON');
    b.addCmakeVariable('CMAKE_CXX_STANDARD', '11');
    b.addCmakeVariable('CMAKE_CXX_STANDARD_REQUIRED', 'ON');

    const cWarnings = (t: {
        addCompileOptions(opts: { opts: string[]; scope: string }): void;
    }) => {
        t.addCompileOptions({
            opts: ['-Wall', '-Wextra', '-Wno-unused-parameter'],
            scope: 'private',
        });
    };

    b.addTarget('sokol', 'interface', (t) => {
        t.setDir('vendor/sokol');
        t.addIncludeDirectories({
            dirs: ['.', './util'],
            scope: 'interface',
        });
        t.addCompileDefinitions({ SOKOL_GLCORE: '1' });
        t.addLibraries(['X11', 'Xi', 'Xcursor', 'GL', 'dl', 'm', 'pthread']);
    });

    b.addTarget('imgui', 'lib', (t) => {
        t.setDir('vendor/dcimgui/src');
        t.addSources([
            'cimgui.cpp',
            'cimgui_internal.cpp',
            'imgui.cpp',
            'imgui_draw.cpp',
            'imgui_tables.cpp',
            'imgui_widgets.cpp',
            'imgui_demo.cpp',
        ]);
        t.addIncludeDirectories({ dirs: ['.'], scope: 'public' });
        t.addCompileOptions({ opts: ['-w'], scope: 'private' });
    });

    b.addTarget('ink-core', 'lib', (t) => {
        t.setDir('src');
        t.addSources(['geom.c', 'level.c', 'nav.c']);
        t.addIncludeDirectories({ dirs: ['.'], scope: 'public' });
        cWarnings(t);
    });

    b.addTarget('test_level', 'plain-exe', (t) => {
        t.addSources(['tests/test_level.c']);
        t.addDependencies(['ink-core']);
        t.addLibraries(['m']);
        cWarnings(t);
    });

    b.addTarget('editor', 'windowed-exe', (t) => {
        t.setDir('src');
        t.addSources(['main.c', 'editor.c']);
        t.addDependencies(['sokol', 'imgui', 'ink-core']);
        cWarnings(t);
        if (b.isGcc() || b.isClang()) {
            t.addCompileOptions({
                opts: ['-Wno-unused-function'],
                language: 'cxx',
                scope: 'private',
            });
        }
    });
}
