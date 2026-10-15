// Searches the pseudocode for patterns that depend on the framerate (docs/research/06-framerate.md):
//   - frame counters: X = X + 1 / X = X - 1 on a field of an object
//   - per-frame lerps without dt:  X = X + (T - X) * k   or   X = (T - X) * k + X
//   - per-frame damping: X = X * 0.9x
//   - constants 1/30, 30.0, 1/60, 60.0
//   - use of the game clock (ns / 1e9), to separate code with dt from code that does not use it
// Usage: -postScript ScanFrameDeps.java <output.tsv> <details.txt> <function_list.txt>
//   (the list: one main offset in hex per line; the image base is added)
// @category PLARecomp

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class ScanFrameDeps extends GhidraScript {

    private static final Pattern COUNTER = Pattern.compile(
        "(\\*\\((?:int|uint|short|ushort|char|byte|undefined4|undefined2|undefined1) \\*\\)\\([^;=]+?\\)) = \\1 [+-] 1;");
    private static final Pattern LERP1 = Pattern.compile(
        "(\\S[^;=]*?) = \\1 \\+ \\(([^;]+?) - \\1\\) \\* ([^;]+);");
    private static final Pattern LERP2 = Pattern.compile(
        "(\\S[^;=]*?) = \\(([^;]+?) - \\1\\) \\* ([^;]+?) \\+ \\1;");
    private static final Pattern DAMP = Pattern.compile(
        "(\\*\\(float \\*\\)\\([^;=]+?\\)) = \\1 \\* (0\\.[5-9]\\d*);");
    private static final Pattern K30 = Pattern.compile("0\\.033333335|\\b30\\.0\\b|29\\.999998|0\\.016666668|\\b60\\.0\\b");
    private static final Pattern DT = Pattern.compile("/ 1000000000\\.0|1e-09");

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        List<Long> funcs = new ArrayList<>();
        for (String l : Files.readAllLines(new File(args[2]).toPath())) {
            l = l.trim();
            if (!l.isEmpty() && !l.startsWith("#")) {
                funcs.add(Long.parseUnsignedLong(l.replace("0x", ""), 16));
            }
        }
        DecompInterface dec = new DecompInterface();
        dec.setOptions(new DecompileOptions());
        dec.openProgram(currentProgram);
        int done = 0;
        try (PrintWriter tsv = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8);
             PrintWriter det = new PrintWriter(new File(args[1]), StandardCharsets.UTF_8)) {
            tsv.println("func\tname\tsize\tuses_dt\tcounters\tlerps\tdamps\tk30");
            for (long off : funcs) {
                if (monitor.isCancelled()) {
                    break;
                }
                Function f = getFunctionAt(toAddr(base + off));
                if (f == null) {
                    f = getFunctionContaining(toAddr(base + off));
                }
                if (f == null) {
                    continue;
                }
                DecompileResults res = dec.decompileFunction(f, 30, monitor);
                if (res == null || !res.decompileCompleted()) {
                    tsv.printf("0x%x\t%s\t%d\t?\t?\t?\t?\t?%n", off, f.getName(), f.getBody().getNumAddresses());
                    continue;
                }
                String c = res.getDecompiledFunction().getC();
                boolean dt = DT.matcher(c).find();
                List<String> hits = new ArrayList<>();
                int nc = count(COUNTER, c, "contador", hits);
                int nl = count(LERP1, c, "lerp", hits) + count(LERP2, c, "lerp", hits);
                int nd = count(DAMP, c, "damping", hits);
                int nk = count(K30, c, "constante", hits);
                tsv.printf("0x%x\t%s\t%d\t%d\t%d\t%d\t%d\t%d%n", off, f.getName(), f.getBody().getNumAddresses(),
                    dt ? 1 : 0, nc, nl, nd, nk);
                if (!hits.isEmpty()) {
                    det.printf("== 0x%x %s dt=%d%n", off, f.getName(), dt ? 1 : 0);
                    for (String h : new LinkedHashSet<>(hits)) {
                        det.println("   " + h);
                    }
                }
                if (++done % 200 == 0) {
                    tsv.flush();
                    det.flush();
                    println("ScanFrameDeps: " + done + "/" + funcs.size());
                }
            }
        }
        dec.dispose();
        println("ScanFrameDeps: " + done + " functions -> " + args[0]);
    }

    private static int count(Pattern p, String c, String tag, List<String> hits) {
        Matcher m = p.matcher(c);
        int n = 0;
        while (m.find()) {
            n++;
            String s = m.group(0).trim();
            if (s.length() > 160) {
                s = s.substring(0, 160) + "...";
            }
            // constant line: show the full line of code
            if (tag.equals("constante")) {
                int a = c.lastIndexOf('\n', m.start()) + 1;
                int b = c.indexOf('\n', m.end());
                s = c.substring(a, b < 0 ? c.length() : b).trim();
                if (s.length() > 160) {
                    s = s.substring(0, 160) + "...";
                }
            }
            hits.add(tag + ": " + s);
        }
        return n;
    }
}
