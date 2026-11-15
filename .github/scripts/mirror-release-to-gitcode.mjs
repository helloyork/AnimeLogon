#!/usr/bin/env node
/**
 * Copy one release's setup program and checksum list to the same-named release on GitCode.
 *
 *   node .github/scripts/mirror-release-to-gitcode.mjs --tag v1.0.0 --from <dir>
 *
 * GitHub is unreachable or crawls from much of mainland China; GitCode serves release files from a
 * CDN inside China. The repository there (https://gitcode.com/nominaawa/AnimeLogon) holds a README
 * that points back here and nothing else: this copies installers, never code. The release is created
 * against that README's branch, so the source archives GitCode attaches to every release contain
 * only the README.
 *
 * Two files go up: AnimeLogon-<version>-setup.exe and SHA256SUMS.txt, byte for byte the ones on the
 * GitHub release. The setup program is checked against its line in SHA256SUMS.txt before anything
 * is sent, and the list goes up last, so it never names a file the release does not have yet.
 *
 * GitCode hands out a presigned storage address for each file, which takes a PUT with a
 * Content-Length. A release file is only reachable by GET (HEAD answers 401) and the release listing
 * carries no sizes, so each upload is confirmed by asking for its first byte and reading the total
 * from Content-Range.
 *
 * The upload crosses from wherever this runs into GitCode's storage in China, and how fast that goes
 * swings by a factor of a hundred over the day: tens of kilobytes a second in the daytime there,
 * megabytes a second in the small hours. So a connection that has carried almost nothing for five
 * minutes is dropped and the file started again on a fresh address, and every minute the upload says
 * how far it has got, because a run that is quiet for an hour cannot be told from one that is stuck.
 *
 * Re-running is safe: a file already there at the right size is skipped.
 *
 * The token comes from GITCODE_TOKEN, or from --token-file (one line; lines starting with # are
 * ignored). It is sent only to api.gitcode.com and never printed.
 */
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import https from "node:https";
import path from "node:path";

const OWNER = "nominaawa";
const REPO = "AnimeLogon";
const API = process.env.GITCODE_API_URL || "https://api.gitcode.com/api/v5";
const SITE = process.env.GITCODE_SITE_URL || "https://gitcode.com";
/** The branch the README-only repository was created with; releases are tagged against it. */
const TARGET_BRANCH = "main";
const GITHUB_RELEASE = "https://github.com/helloyork/AnimeLogon/releases/tag";
const CHECKSUMS = "SHA256SUMS.txt";
const UPLOAD_ATTEMPTS = 3;
/** An upload slower than this for {@link STALL_WINDOW_MS} is abandoned and started again. */
const STALL_BYTES_PER_SECOND = 10 * 1024;
const STALL_WINDOW_MS = 5 * 60_000;
const PROGRESS_EVERY_MS = 60_000;

function parseArgs(argv) {
    const args = {};
    for (let index = 0; index < argv.length; index += 1) {
        const key = argv[index];
        if (!key.startsWith("--")) {
            throw new Error(`Unexpected argument ${key}`);
        }
        args[key.slice(2)] = argv[index + 1];
        index += 1;
    }
    return args;
}

function readToken(args) {
    if (args["token-file"]) {
        const lines = fs.readFileSync(args["token-file"], "utf8")
            .split(/\r?\n/)
            .map(line => line.trim())
            .filter(line => line && !line.startsWith("#"));
        if (lines.length !== 1) {
            throw new Error(`${args["token-file"]} should hold exactly one token line.`);
        }
        return lines[0];
    }
    if (process.env.GITCODE_TOKEN) {
        return process.env.GITCODE_TOKEN.trim();
    }
    throw new Error("No GitCode token: set GITCODE_TOKEN or pass --token-file.");
}

function api(token) {
    return async (method, route, body) => {
        const response = await fetch(`${API}${route}`, {
            method,
            headers: {
                Authorization: `Bearer ${token}`,
                ...(body ? { "Content-Type": "application/json; charset=utf-8" } : {}),
            },
            body: body ? JSON.stringify(body) : undefined,
            signal: AbortSignal.timeout(60_000),
        });
        const text = await response.text();
        let json = null;
        try {
            json = text ? JSON.parse(text) : null;
        } catch {
            // Reported below with the status.
        }
        return { status: response.status, json, text };
    };
}

/** The total size GitCode serves for a release file, or null when it does not serve it. */
async function servedSize(tag, name) {
    const url = `${SITE}/${OWNER}/${REPO}/releases/download/${encodeURIComponent(tag)}/${encodeURIComponent(name)}`;
    try {
        const response = await fetch(url, { headers: { Range: "bytes=0-0" }, signal: AbortSignal.timeout(60_000) });
        await response.body?.cancel();
        if (response.status === 206) {
            const total = /\/(\d+)$/.exec(response.headers.get("content-range") ?? "");
            return total ? Number(total[1]) : null;
        }
        if (response.status === 200) {
            const length = response.headers.get("content-length");
            return length === null ? null : Number(length);
        }
        return null;
    } catch {
        return null;
    }
}

/** `servedSize` a few times over a minute: GitCode lists a file a moment after its upload ends. */
async function settledSize(tag, name, expected) {
    let served = null;
    for (let check = 0; check < 6; check += 1) {
        served = await servedSize(tag, name);
        if (served === expected) {
            return served;
        }
        await new Promise(resolve => setTimeout(resolve, 10_000));
    }
    return served;
}

/**
 * PUT a file to the presigned address GitCode hands out, streamed from disk with the Content-Length
 * the storage behind it insists on (a streamed fetch body has none). Counts what it sends, so it can
 * say how far it has got and give up on a connection that has stopped carrying anything.
 */
function putFile(file, url, headers, name) {
    const size = fs.statSync(file).size;
    const target = new URL(url);
    const client = target.protocol === "http:" ? http : https;
    return new Promise(resolve => {
        const started = Date.now();
        let sent = 0;
        let windowStart = started;
        let windowSent = 0;
        let finished = false;
        const finish = result => {
            if (finished) {
                return;
            }
            finished = true;
            clearInterval(watch);
            // An abandoned upload must not go on reading the file into a closed connection.
            body.destroy();
            resolve(result);
        };
        const request = client.request(target, {
            method: "PUT",
            headers: { ...(headers ?? {}), "Content-Length": String(size) },
        }, response => {
            response.resume();
            response.on("end", () => finish({
                ok: response.statusCode >= 200 && response.statusCode < 300,
                status: String(response.statusCode),
                error: "",
            }));
        });
        request.on("error", error => finish({ ok: false, status: "", error: error.message }));
        const body = fs.createReadStream(file);
        body.on("data", chunk => {
            sent += chunk.length;
            windowSent += chunk.length;
        });
        body.pipe(request);

        let lastReport = started;
        const watch = setInterval(() => {
            const now = Date.now();
            if (now - lastReport >= PROGRESS_EVERY_MS) {
                lastReport = now;
                const rate = sent / Math.max((now - started) / 1000, 0.001);
                console.log(`  ${name}: ${formatSize(sent)} of ${formatSize(size)} (${formatSize(rate)}/s)`);
            }
            if (now - windowStart >= STALL_WINDOW_MS) {
                if (windowSent / ((now - windowStart) / 1000) < STALL_BYTES_PER_SECOND && sent < size) {
                    request.destroy(new Error(`under ${formatSize(STALL_BYTES_PER_SECOND)}/s for ${STALL_WINDOW_MS / 60_000} minutes`));
                    return;
                }
                windowStart = now;
                windowSent = 0;
            }
        }, 5_000);
    });
}

function formatSize(bytes) {
    return bytes >= 1 << 20 ? `${(bytes / (1 << 20)).toFixed(1)} MB` : `${(bytes / 1024).toFixed(0)} KB`;
}

/** The setup program in `from`, checked against its line in SHA256SUMS.txt. */
function setupProgram(from) {
    const setups = fs.readdirSync(from).filter(name => name.endsWith("-setup.exe"));
    if (setups.length !== 1) {
        throw new Error(`Expected one *-setup.exe in ${from}, found ${setups.length}.`);
    }
    const list = path.join(from, CHECKSUMS);
    if (!fs.existsSync(list)) {
        throw new Error(`No ${CHECKSUMS} in ${from}.`);
    }
    const name = setups[0];
    const listed = fs.readFileSync(list, "utf8").split(/\r?\n/)
        .map(line => /^([0-9a-fA-F]{64}) [ *]?(.+)$/.exec(line.trim()))
        .find(match => match && match[2] === name)?.[1]?.toLowerCase();
    const actual = crypto.createHash("sha256").update(fs.readFileSync(path.join(from, name))).digest("hex");
    if (listed !== actual) {
        throw new Error(`${name} does not match ${CHECKSUMS} (listed ${listed ?? "nothing"}, file ${actual}).`);
    }
    return path.join(from, name);
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const tag = args.tag;
    const from = args.from;
    if (!tag || !/^v\d+\.\d+\.\d+/.test(tag) || !from) {
        throw new Error("Usage: --tag vX.Y.Z --from <directory holding the release's files> [--token-file <file>]");
    }
    const call = api(readToken(args));
    const files = [setupProgram(from), path.join(from, CHECKSUMS)];

    const release = await call("GET", `/repos/${OWNER}/${REPO}/releases/tags/${encodeURIComponent(tag)}`);
    if (release.status === 404 || (release.status === 400 && /release/i.test(release.text))) {
        console.log(`Creating the ${tag} release on GitCode.`);
        const created = await call("POST", `/repos/${OWNER}/${REPO}/releases`, {
            tag_name: tag,
            name: `AnimeLogon ${tag.slice(1)}`,
            body: `AnimeLogon ${tag.slice(1)}\n\n与 GitHub 上的同名发行版是同一批文件，供无法顺畅访问 GitHub 时下载。发布说明和源代码见 ${GITHUB_RELEASE}/${tag}\n\nThe same files as the GitHub release of the same name. Release notes and source code: ${GITHUB_RELEASE}/${tag}`,
            target_commitish: TARGET_BRANCH,
        });
        if (created.status < 200 || created.status >= 300) {
            throw new Error(`Creating the release answered ${created.status}: ${created.text.slice(0, 500)}`);
        }
    } else if (release.status !== 200) {
        throw new Error(`Reading the release answered ${release.status}: ${release.text.slice(0, 500)}`);
    }

    for (const file of files) {
        const name = path.basename(file);
        const size = fs.statSync(file).size;
        const already = await servedSize(tag, name);
        if (already === size) {
            console.log(`= ${name} (${formatSize(size)}) is already there.`);
            continue;
        }
        if (already !== null) {
            throw new Error(`${name} is already on GitCode at ${already} bytes, not ${size}. Delete it from the release page and run this again.`);
        }

        let done = false;
        for (let attempt = 1; attempt <= UPLOAD_ATTEMPTS && !done; attempt += 1) {
            const target = await call("GET", `/repos/${OWNER}/${REPO}/releases/${encodeURIComponent(tag)}/upload_url?file_name=${encodeURIComponent(name)}`);
            const url = target.json?.url;
            if (!url) {
                console.log(`  ${name}: no upload address (answered ${target.status}), attempt ${attempt}/${UPLOAD_ATTEMPTS}`);
                continue;
            }
            console.log(`  ${name}: uploading ${formatSize(size)}${attempt > 1 ? `, attempt ${attempt}/${UPLOAD_ATTEMPTS}` : ""}`);
            const started = Date.now();
            const put = await putFile(file, url, target.json.headers, name);
            const seconds = Math.max((Date.now() - started) / 1000, 0.001);
            if (!put.ok) {
                console.log(`  ${name}: upload answered ${put.status || "nothing"} ${put.error}, attempt ${attempt}/${UPLOAD_ATTEMPTS}`);
                continue;
            }
            const served = await settledSize(tag, name, size);
            if (served !== size) {
                console.log(`  ${name}: GitCode serves ${served ?? "nothing"} bytes after the upload, expected ${size}, attempt ${attempt}/${UPLOAD_ATTEMPTS}`);
                continue;
            }
            console.log(`+ ${name} (${formatSize(size)}, ${formatSize(size / seconds)}/s)`);
            done = true;
        }
        if (!done) {
            throw new Error(`${name} did not reach GitCode after ${UPLOAD_ATTEMPTS} attempts.`);
        }
    }
    console.log(`${tag} is on GitCode: ${SITE}/${OWNER}/${REPO}/releases/${tag}`);
}

main().catch(error => {
    console.error(`::error::${error.message}`);
    process.exitCode = 1;
});
