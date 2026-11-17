// SPDX-License-Identifier: MIT
import fs from "node:fs";
import path from "node:path";
import { dlopen, linkSymbols, FFIType, ptr, toArrayBuffer } from "bun:ffi";

const spec = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const report = { schema: "jianying-bun-codec/v1", status: "failed", operation: spec.operation,
  bun_version: Bun.version };
const keepAlive = [];

// MSVC x64 std::string has a 16-byte union followed by size and capacity.
// Native allocations belong to this one-shot process, never to the Bun allocator.
function stringArg(data) {
  const object = Buffer.alloc(32);
  let payload = null;
  object.writeBigUInt64LE(BigInt(data.length), 16);
  if (data.length < 16) {
    data.copy(object);
    object.writeBigUInt64LE(15n, 24);
  } else {
    payload = Buffer.concat([data, Buffer.from([0])]);
    object.writeBigUInt64LE(BigInt(ptr(payload)), 0);
    object.writeBigUInt64LE(BigInt(data.length), 24);
  }
  keepAlive.push(object, payload);
  return { object, payload };
}

function outputString() {
  const object = Buffer.alloc(32);
  object.writeBigUInt64LE(15n, 24);
  keepAlive.push(object);
  return object;
}

function stringBytes(object) {
  const size = object.readBigUInt64LE(16);
  const capacity = object.readBigUInt64LE(24);
  if (size > capacity || size > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new Error("Invalid native std::string size/capacity");
  }
  if (capacity < 16n) return Buffer.from(object.subarray(0, Number(size)));
  if (size === 0n) return Buffer.alloc(0);
  const address = Number(object.readBigUInt64LE(0));
  if (!address) throw new Error("Native std::string has no payload pointer");
  return Buffer.from(toArrayBuffer(address, 0, Number(size)));
}

try {
  if (process.platform !== "win32" || process.arch !== "x64") {
    throw new Error("This bridge requires Windows x64 and the MSVC x64 std::string ABI");
  }
  process.chdir(spec.install_dir);
  const win = dlopen("kernel32.dll", {
    LoadLibraryW: { args: [FFIType.ptr], returns: FFIType.ptr },
    GetProcAddress: { args: [FFIType.ptr, FFIType.ptr], returns: FFIType.ptr },
  });
  const dllPath = Buffer.from(path.join(spec.install_dir, "videoeditor.dll") + "\0", "utf16le");
  keepAlive.push(dllPath);
  const handle = win.symbols.LoadLibraryW(ptr(dllPath));
  if (!handle) throw new Error("LoadLibraryW(videoeditor.dll) failed");
  function address(name) {
    const symbol = Buffer.from(name + "\0");
    keepAlive.push(symbol);
    const value = win.symbols.GetProcAddress(handle, ptr(symbol));
    if (!value) throw new Error("Required videoeditor.dll named export not found: " + name);
    return value;
  }
  // Bun compiles aliases as C identifiers; decorated C++ names cannot be used directly.
  const api = linkSymbols({
    decrypt: {
      ptr: address("?decrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@0AEA_N@Z"),
      args: [FFIType.ptr, FFIType.ptr, FFIType.ptr, FFIType.ptr, FFIType.ptr], returns: FFIType.ptr,
    },
    encrypt: {
      ptr: address("?encrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@@Z"),
      args: [FFIType.ptr, FFIType.ptr, FFIType.ptr], returns: FFIType.ptr,
    },
    enable: { ptr: address("?enable@EncryptUtils@lvve@@QEAAX_N@Z"),
      args: [FFIType.ptr, FFIType.bool], returns: FFIType.void },
  });
  const parameter = stringArg(Buffer.from("{}"));
  function decrypt(data) {
    const input = stringArg(data);
    const output = outputString();
    const ok = Buffer.alloc(1);
    keepAlive.push(ok);
    api.symbols.decrypt(null, ptr(output), ptr(input.object), ptr(parameter.object), ptr(ok));
    const bytes = stringBytes(output);
    if (ok[0] !== 1 || bytes.length === 0) {
      throw new Error(`decrypt failed: ok_flag=${ok[0]}, output_len=${bytes.length}`);
    }
    return bytes;
  }
  const input = fs.readFileSync(spec.input);
  let output;
  if (spec.operation === "decode") {
    output = decrypt(input);
  } else if (spec.operation === "encode") {
    const argument = stringArg(input);
    const encrypted = outputString();
    api.symbols.enable(null, true);
    api.symbols.encrypt(null, ptr(encrypted), ptr(argument.object));
    output = stringBytes(encrypted);
    if (!output.length || output.equals(input)) throw new Error("encrypt did not produce encrypted content");
    report.roundtrip_equal = decrypt(output).equals(input);
    if (!report.roundtrip_equal) throw new Error("encrypt/decrypt round-trip changed draft bytes");
  } else {
    throw new Error("Unsupported codec operation: " + spec.operation);
  }
  fs.writeFileSync(spec.output, output);
  report.output_bytes = output.length;
  report.status = "passed";
} catch (error) {
  report.error = String(error);
} finally {
  fs.writeFileSync(spec.report, JSON.stringify(report), "utf8");
}
process.exitCode = report.status === "passed" ? 0 : 1;
