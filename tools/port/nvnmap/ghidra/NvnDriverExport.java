// Names the driver's 523 NVN functions in sdk (docs/port/data/nvn_sdk_map.csv) and exports their
// pseudo-C and their callees' (up to <depth> levels) to <outDir>/<name or FUN_xxx>.c.
// Usage: -postScript NvnDriverExport.java <map.csv> <outDir> <depth>
// @category PLARecomp
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;

public class NvnDriverExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File csv = new File(a[0]);
        File out = new File(a[1]);
        int depth = Integer.parseInt(a[2]);
        out.mkdirs();
        Map<Long, String> names = new LinkedHashMap<>();
        List<String> lines = Files.readAllLines(csv.toPath(), StandardCharsets.UTF_8);
        for (int i = 1; i < lines.size(); i++) {
            String[] f = lines.get(i).split(",");
            names.put(Long.decode(f[2]), f[0]);
        }
        FunctionManager fm = currentProgram.getFunctionManager();
        for (Map.Entry<Long, String> e : names.entrySet()) {
            Address ad = toAddr(e.getKey());
            Function fn = fm.getFunctionAt(ad);
            if (fn == null) {
                disassemble(ad);
                fn = createFunction(ad, e.getValue());
            }
            if (fn != null) fn.setName(e.getValue(), SourceType.USER_DEFINED);
        }
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        Set<Long> seen = new HashSet<>();
        Deque<long[]> q = new ArrayDeque<>();
        for (Long k : names.keySet()) q.add(new long[]{k, 0});
        StringBuilder index = new StringBuilder();
        while (!q.isEmpty()) {
            long[] it = q.poll();
            if (!seen.add(it[0])) continue;
            Function fn = fm.getFunctionAt(toAddr(it[0]));
            if (fn == null) continue;
            DecompileResults r = ifc.decompileFunction(fn, 60, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("// error: " + r.getErrorMessage());
            String nm = fn.getName();
            Files.write(new File(out, nm + ".c").toPath(), c.getBytes(StandardCharsets.UTF_8));
            index.append(String.format("0x%x,%s,%d%n", it[0], nm, it[1]));
            if (it[1] < depth) {
                for (Function callee : fn.getCalledFunctions(monitor)) {
                    if (callee.isThunk() || callee.isExternal()) continue;
                    q.add(new long[]{callee.getEntryPoint().getOffset(), it[1] + 1});
                }
            }
        }
        Files.write(new File(out, "_index.csv").toPath(), index.toString().getBytes(StandardCharsets.UTF_8));
        println("NvnDriverExport: " + seen.size() + " functions");
    }
}
