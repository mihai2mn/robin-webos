// SPDX-License-Identifier: GPL-3.0-or-later
// uBlock Origin's scriptlets (src/js/resources/scriptlets.js, an ES module) -> adblock-rust resources (JSON).
//
//     node scriptlets-to-json.mjs UBO_SRC/js/resources/scriptlets.js OUT.json
//
// Each scriptlet is a JS function; adblock-rust injects its source and calls it by name with the filter's
// arguments, after the sources of its dependencies (the ".fn" helpers). Scriptlets uBO marks `requiresTrust`
// (trusted-set-constant, trusted-replace-fetch-response, ...) get permission bit 0, which only uBO's own lists
// carry (engine/ubo/src/bin/compile.rs, `:trusted`), as in uBO.
import { writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const [ src, out ] = process.argv.slice(2);
if ( !src || !out ) {
    console.error('usage: node scriptlets-to-json.mjs scriptlets.js OUT.json');
    process.exit(2);
}
const { builtinScriptlets } = await import(pathToFileURL(resolve(src)).href);
// Wren's own scriptlets (tools/ubo/wren-scriptlets.js), next to this script.
const { wrenScriptlets } = await import(new URL('./wren-scriptlets.js', import.meta.url).href);

const resources = [];
const skipped = [];
for ( const s of [ ...builtinScriptlets, ...wrenScriptlets ] ) {
    if ( typeof s.fn !== 'function' || typeof s.name !== 'string' ) { skipped.push(s.name); continue; }
    let source = s.fn.toString();
    // adblock-rust calls a scriptlet by the name in "function name(". Helpers (".fn") only need to define their
    // name for the scriptlets that use them: classes do as they are, other forms get a declaration.
    if ( /^function\s*[\w$]+\s*\(/.test(source) === false ) {
        if ( s.name.endsWith('.fn') === false || !s.fn.name ) { skipped.push(s.name); continue; }
        if ( /^class\s/.test(source) === false ) { source = `var ${s.fn.name} = ${source};`; }
    }
    resources.push({
        name: s.name,
        aliases: Array.isArray(s.aliases) ? s.aliases : [],
        kind: { mime: s.name.endsWith('.fn') ? 'fn/javascript' : 'application/javascript' },
        content: Buffer.from(source).toString('base64'),
        dependencies: Array.isArray(s.dependencies) ? s.dependencies : [],
        permission: s.requiresTrust ? 1 : 0,
    });
}
writeFileSync(out, JSON.stringify(resources));
console.error(`scriptlets: ${resources.length} written, ${skipped.length} skipped${skipped.length ? ' (' + skipped.join(', ') + ')' : ''}`);
