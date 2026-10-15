// Probe for the frame time analysis of the game (docs/research/06-framerate.md).
// Decompiles functions and lists references from a list of addresses.
//
// Usage (headless, project already analyzed and read-only):
//   analyzeHeadless tools/ghidra/projects main -process main.elf -noanalysis -readOnly
//     -scriptPath tools/ghidra/scripts
//     -postScript FramerateProbe.java <output.txt> <command> [<command> ...]
//
// Commands (addresses are offsets of the main module; the image base is added):
//   dec:<hex>      decompiles the function containing <hex> and lists who calls it
//   xref:<hex>     references to <hex> (code or data) with the function containing them
//   dis:<hex>:<n>  disassembles <n> instructions from <hex>
//   @<file>        reads commands from a file (one per line, '#' = comment)
// @category PLARecomp

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FramerateProbe extends GhidraScript {

    private DecompInterface dec;
    private long base;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: FramerateProbe.java <output> <command>...");
            return;
        }
        base = currentProgram.getImageBase().getOffset();
        List<String> cmds = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                for (String l : Files.readAllLines(new File(args[i].substring(1)).toPath())) {
                    l = l.trim();
                    if (!l.isEmpty() && !l.startsWith("#")) {
                        cmds.add(l);
                    }
                }
            } else {
                cmds.add(args[i]);
            }
        }
        dec = new DecompInterface();
        DecompileOptions opt = new DecompileOptions();
        dec.setOptions(opt);
        dec.toggleCCode(true);
        dec.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8)) {
            for (String c : cmds) {
                if (monitor.isCancelled()) {
                    break;
                }
                String[] p = c.split(":");
                try {
                    switch (p[0]) {
                        case "dec" -> decompile(w, parse(p[1]));
                        case "xref" -> xrefs(w, parse(p[1]));
                        case "dis" -> disasm(w, parse(p[1]), p.length > 2 ? Integer.parseInt(p[2]) : 32);
                        case "grep" -> decgrep(w, parse(p[1]), p[2], p.length > 3 ? Integer.parseInt(p[3]) : 4);
                        default -> w.printf("## orden desconocida: %s%n", c);
                    }
                } catch (Exception e) {
                    w.printf("## error in %s: %s%n", c, e);
                }
                w.flush();
            }
        }
        dec.dispose();
        println("FramerateProbe: " + cmds.size() + " commands -> " + args[0]);
    }

    private Address parse(String s) {
        long v = Long.parseUnsignedLong(s.replace("0x", ""), 16);
        if (v < base) {
            v += base;
        }
        return toAddr(v);
    }

    private String off(Address a) {
        return String.format("main+0x%x", a.getOffset() - base);
    }

    private String fname(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "?" : f.getName() + "@" + off(f.getEntryPoint());
    }

    private void xrefs(PrintWriter w, Address a) {
        w.printf("%n==== xref %s%n", off(a));
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
        int n = 0;
        while (it.hasNext()) {
            Reference r = it.next();
            w.printf("  %s  %-10s  in %s%n", off(r.getFromAddress()), r.getReferenceType(), fname(r.getFromAddress()));
            if (++n >= 200) {
                w.println("  ...");
                break;
            }
        }
    }

    private void disasm(PrintWriter w, Address a, int n) {
        w.printf("%n==== dis %s (%s)%n", off(a), fname(a));
        Instruction ins = getInstructionAt(a);
        if (ins == null) {
            ins = getInstructionAfter(a);
        }
        for (int i = 0; i < n && ins != null; i++, ins = ins.getNext()) {
            w.printf("  %s  %s%n", off(ins.getAddress()), ins);
        }
    }

    // Decompiles the function containing <a> and shows only the lines matching <regex>
    // (with <ctx> lines of context). Useful for huge functions.
    private final Set<Long> grepped = new java.util.HashSet<>();

    private void decgrep(PrintWriter w, Address a, String regex, int ctx) {
        Function f = getFunctionContaining(a);
        if (f == null) {
            w.printf("%n==== grep %s: no function%n", off(a));
            return;
        }
        if (!grepped.add(f.getEntryPoint().getOffset())) {
            w.printf("%n==== grep %s -> %s (already listed)%n", off(a), fname(f.getEntryPoint()));
            return;
        }
        Set<String> callers = new LinkedHashSet<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext() && callers.size() < 12) {
            Reference r = it.next();
            callers.add(off(r.getFromAddress()) + "(" + fname(r.getFromAddress()) + ")");
        }
        w.printf("%n==== grep %s -> %s  size=%d%n", off(a), fname(f.getEntryPoint()), f.getBody().getNumAddresses());
        w.printf("  llamadores: %s%n", String.join(" ", callers));
        DecompileResults res = dec.decompileFunction(f, 180, monitor);
        if (res == null || !res.decompileCompleted()) {
            w.printf("  (could not decompile)%n");
            return;
        }
        String[] lines = res.getDecompiledFunction().getC().split("\n");
        java.util.regex.Pattern pat = java.util.regex.Pattern.compile(regex);
        boolean[] show = new boolean[lines.length];
        for (int i = 0; i < lines.length; i++) {
            if (pat.matcher(lines[i]).find()) {
                for (int j = Math.max(0, i - ctx); j <= Math.min(lines.length - 1, i + ctx); j++) {
                    show[j] = true;
                }
            }
        }
        // the signature always
        for (int i = 0; i < lines.length; i++) {
            if (lines[i].contains("(") && !lines[i].startsWith(" ")) {
                w.println(lines[i]);
                break;
            }
        }
        boolean gap = false;
        for (int i = 0; i < lines.length; i++) {
            if (show[i]) {
                w.printf("%5d| %s%n", i, lines[i]);
                gap = false;
            } else if (!gap) {
                w.println("  ...");
                gap = true;
            }
        }
    }

    private void decompile(PrintWriter w, Address a) {
        Function f = getFunctionContaining(a);
        if (f == null) {
            w.printf("%n==== dec %s: no function%n", off(a));
            return;
        }
        Set<String> callers = new LinkedHashSet<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext() && callers.size() < 40) {
            Reference r = it.next();
            callers.add(off(r.getFromAddress()) + "(" + fname(r.getFromAddress()) + "," + r.getReferenceType() + ")");
        }
        w.printf("%n==== dec %s -> %s  size=%d%n", off(a), fname(f.getEntryPoint()), f.getBody().getNumAddresses());
        w.printf("  llamadores: %s%n", String.join(" ", callers));
        DecompileResults res = dec.decompileFunction(f, 120, monitor);
        if (res != null && res.decompileCompleted()) {
            w.println(res.getDecompiledFunction().getC());
        } else {
            w.printf("  (could not decompile: %s)%n", res == null ? "null" : res.getErrorMessage());
        }
    }
}
