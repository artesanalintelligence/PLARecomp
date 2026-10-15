// Exports Ghidra's decompiler pseudo-C, one function per file:
//   <outDir>/<address without 0x>.c   (e.g. 7100123456.c)
//   <outDir>/<address>.err            if the decompiler fails or times out
//
// Meant to run in batch (all functions, in the order of a priority file) and
// at the same time serve on-demand requests: while running, it reads
// <outDir>/_requests/*.req (one hexadecimal address per line), puts them at the
// front of the queue and deletes the .req. It writes a heartbeat to <outDir>/_server.json
// so that tools/decomp/ctx.py knows an active server exists.
// It is resumable: it skips the functions that already have a .c (or .err).
//
// Usage (launched by tools/decomp/ghidra_decomp.py):
//   -postScript ExportDecompiled.java <outDir> <orderFile|all|none> <threads> <timeoutSec> [serve]
//   "serve": when the queue is finished, keep waiting for requests until <outDir>/_stop exists.
// @category PLARecomp

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.util.task.TaskMonitor;

public class ExportDecompiled extends GhidraScript {

    private final LinkedBlockingDeque<Long> queue = new LinkedBlockingDeque<>();
    private final AtomicInteger busy = new AtomicInteger();
    private final AtomicLong done = new AtomicLong();
    private final AtomicLong failed = new AtomicLong();
    private final AtomicLong skipped = new AtomicLong();
    private volatile boolean stopping = false;
    private File outDir;
    private int timeout;

    private File outFile(long a, String ext) {
        return new File(outDir, Long.toHexString(a) + ext);
    }

    private void writeAtomic(File f, String text) throws IOException {
        File tmp = new File(f.getPath() + ".tmp" + Thread.currentThread().getId());
        Files.write(tmp.toPath(), text.getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    private void worker() {
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(true);
        ifc.setSimplificationStyle("decompile");
        if (!ifc.openProgram(currentProgram)) {
            printerr("could not open the decompiler: " + ifc.getLastMessage());
            return;
        }
        int sinceReset = 0;
        try {
            while (!stopping) {
                Long a;
                try {
                    a = queue.pollFirst(500, java.util.concurrent.TimeUnit.MILLISECONDS);
                } catch (InterruptedException e) {
                    break;
                }
                if (a == null) {
                    continue;
                }
                busy.incrementAndGet();
                try {
                    decompileOne(ifc, a);
                } catch (Throwable t) {
                    failed.incrementAndGet();
                    try {
                        writeAtomic(outFile(a, ".err"), "exception: " + t + "\n");
                    } catch (IOException e) {
                        // nada
                    }
                } finally {
                    busy.decrementAndGet();
                }
                // The decompiler's native process accumulates memory: restart it from time to time.
                if (++sinceReset >= 2000) {
                    ifc.resetDecompiler();
                    sinceReset = 0;
                }
            }
        } finally {
            ifc.dispose();
        }
    }

    private void decompileOne(DecompInterface ifc, long a) throws IOException {
        File c = outFile(a, ".c");
        if (c.exists() || outFile(a, ".err").exists()) {
            skipped.incrementAndGet();
            return;
        }
        Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
        Function f = currentProgram.getFunctionManager().getFunctionAt(addr);
        if (f == null) {
            f = currentProgram.getFunctionManager().getFunctionContaining(addr);
        }
        if (f == null) {
            writeAtomic(outFile(a, ".err"), "no Ghidra function at 0x" + Long.toHexString(a) + "\n");
            failed.incrementAndGet();
            return;
        }
        DecompileResults res = ifc.decompileFunction(f, timeout, TaskMonitor.DUMMY);
        DecompiledFunction df = res == null ? null : res.getDecompiledFunction();
        if (df == null) {
            String msg = res == null ? "no result" : res.getErrorMessage();
            writeAtomic(outFile(a, ".err"), "decompile failed: " + msg + "\n");
            failed.incrementAndGet();
            return;
        }
        StringBuilder sb = new StringBuilder();
        sb.append("// Ghidra ").append(getGhidraVersion()).append(" | ").append(f.getName(true))
          .append(" @ 0x").append(Long.toHexString(f.getEntryPoint().getOffset()))
          .append(" | cuerpo ").append(f.getBody().getNumAddresses()).append(" bytes\n");
        if (f.getEntryPoint().getOffset() != a) {
            sb.append("// WARNING: 0x").append(Long.toHexString(a)).append(" is inside this Ghidra function\n");
        }
        sb.append("// firma: ").append(df.getSignature()).append("\n");
        sb.append(df.getC());
        writeAtomic(c, sb.toString());
        done.incrementAndGet();
    }

    private void pollRequests(File reqDir) {
        File[] reqs = reqDir.listFiles((d, n) -> n.endsWith(".req"));
        if (reqs == null) {
            return;
        }
        for (File r : reqs) {
            try {
                List<String> lines = Files.readAllLines(r.toPath(), StandardCharsets.UTF_8);
                for (int i = lines.size() - 1; i >= 0; i--) {
                    String s = lines.get(i).trim();
                    if (s.isEmpty()) {
                        continue;
                    }
                    if (s.startsWith("0x") || s.startsWith("0X")) {
                        s = s.substring(2);
                    }
                    queue.addFirst(Long.parseUnsignedLong(s, 16));
                }
            } catch (Exception e) {
                printerr("invalid request " + r + ": " + e);
            }
            r.delete();
        }
    }

    private void heartbeat(File hb, long total, boolean serve) {
        String s = String.format("{\"pid\": %d, \"time\": %d, \"queue\": %d, \"done\": %d, \"failed\": %d, " +
                "\"skipped\": %d, \"total\": %d, \"serve\": %s}\n",
            ProcessHandle.current().pid(), System.currentTimeMillis() / 1000, queue.size(), done.get(),
            failed.get(), skipped.get(), total, serve ? "true" : "false");
        try {
            writeAtomic(hb, s);
        } catch (IOException e) {
            // nada
        }
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) {
            printerr("Usage: ExportDecompiled.java <outDir> <orderFile|all|none> <threads> <timeoutSec> [serve]");
            return;
        }
        outDir = new File(args[0]);
        outDir.mkdirs();
        File reqDir = new File(outDir, "_requests");
        reqDir.mkdirs();
        File hb = new File(outDir, "_server.json");
        File stop = new File(outDir, "_stop");
        stop.delete();
        int threads = Integer.parseInt(args[2]);
        timeout = Integer.parseInt(args[3]);
        boolean serve = args.length > 4 && args[4].equals("serve");

        List<Long> order = new ArrayList<>();
        if (args[1].equals("all")) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                if (!f.isExternal()) {
                    order.add(f.getEntryPoint().getOffset());
                }
            }
        } else if (!args[1].equals("none")) {
            for (String s : Files.readAllLines(Path.of(args[1]), StandardCharsets.UTF_8)) {
                s = s.trim();
                if (s.isEmpty() || s.startsWith("#")) {
                    continue;
                }
                if (s.startsWith("0x") || s.startsWith("0X")) {
                    s = s.substring(2);
                }
                order.add(Long.parseUnsignedLong(s, 16));
            }
        }
        for (Long a : order) {
            if (!outFile(a, ".c").exists() && !outFile(a, ".err").exists()) {
                queue.addLast(a);
            }
        }
        long total = queue.size();
        println("ExportDecompiled: " + total + " functions pending, " + threads + " threads, timeout " + timeout + " s");
        pollRequests(reqDir);

        List<Thread> ws = new ArrayList<>();
        for (int i = 0; i < threads; i++) {
            Thread t = new Thread(this::worker, "decomp-" + i);
            t.setDaemon(true);
            t.start();
            ws.add(t);
        }
        long lastLog = 0;
        while (!monitor.isCancelled()) {
            pollRequests(reqDir);
            heartbeat(hb, total, serve);
            long now = System.currentTimeMillis();
            if (now - lastLog > 60000) {
                println(String.format("progress: %d done, %d failed, %d queued", done.get(), failed.get(), queue.size()));
                lastLog = now;
            }
            if (stop.exists()) {
                break;
            }
            if (queue.isEmpty() && busy.get() == 0 && !serve) {
                break;
            }
            Thread.sleep(1000);
        }
        stopping = true;
        for (Thread t : ws) {
            t.join(timeout * 1000L + 5000);
        }
        hb.delete();
        println(String.format("ExportDecompiled finished: %d done, %d failed, %d skipped", done.get(), failed.get(), skipped.get()));
    }
}
