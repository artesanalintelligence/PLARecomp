// Exports the program's functions in viking's format (nx-decomp-tools):
//   Address,Quality,Size,Name
//   0x0000007100001234,U,000052,_ZN4game4Base6updateEf
// * Quality = U for all (pending).
// * Name = mangled symbol if it exists (what viking expects); otherwise the
//   non-generic Ghidra name (e.g. Class::vf3 applied by ApplyRttiNames) with
//   commas replaced by ';'; empty for FUN_xxx (or FUN_xxx with "fun").
// * Size = contiguous range containing the entry, trimmed to the next function.
// It also writes a TSV with the full (demangled) name and the origin.
// Usage: -postScript ExportFunctionsCsv.java <output.csv> [output.tsv] [empty|fun]
// @category PLARecomp

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ExportFunctionsCsv extends GhidraScript {

    private static class Row {
        long addr;
        long size;
        String name;
        String full;
        String mangled;
        String source;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("Usage: ExportFunctionsCsv.java <output.csv> [output.tsv] [empty|fun]");
            return;
        }
        boolean funNames = args.length > 2 && args[2].equals("fun");
        List<Row> rows = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (f.isExternal()) {
                continue;
            }
            Address entry = f.getEntryPoint();
            if (!entry.isMemoryAddress() || currentProgram.getMemory().getBlock(entry) == null ||
                !currentProgram.getMemory().getBlock(entry).isExecute()) {
                continue;
            }
            AddressRange r = f.getBody().getRangeContaining(entry);
            long size = r == null ? 4 : r.getMaxAddress().subtract(entry) + 1;
            Row row = new Row();
            row.addr = entry.getOffset();
            row.size = size;
            row.mangled = "";
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(entry)) {
                if (s.getName().startsWith("_Z")) {
                    row.mangled = s.getName();
                    break;
                }
            }
            row.full = f.getName(true);
            row.source = f.getSymbol().getSource().toString();
            if (!row.mangled.isEmpty()) {
                row.name = row.mangled;
            }
            else if (f.getSymbol().getSource() == SourceType.DEFAULT) {
                row.name = funNames ? f.getName() : "";
            }
            else {
                row.name = row.full;
            }
            row.name = row.name.replace(", ", ";").replace(",", ";").replace("\n", " ");
            rows.add(row);
        }
        rows.sort((a, b) -> Long.compareUnsigned(a.addr, b.addr));
        Set<String> used = new HashSet<>();
        int truncated = 0;
        try (PrintWriter csv = new PrintWriter(args[0], StandardCharsets.UTF_8)) {
            csv.print("Address,Quality,Size,Name\n");
            for (int i = 0; i < rows.size(); i++) {
                Row r = rows.get(i);
                if (i + 1 < rows.size()) {
                    long room = rows.get(i + 1).addr - r.addr;
                    if (r.size > room) {
                        r.size = room;
                        truncated++;
                    }
                }
                String name = r.name;
                if (!name.isEmpty() && !used.add(name)) {
                    name = name + "@" + Long.toHexString(r.addr);
                    used.add(name);
                }
                csv.printf("0x%016x,U,%06d,%s\n", r.addr, r.size, name);
            }
        }
        if (args.length > 1) {
            try (PrintWriter tsv = new PrintWriter(args[1], StandardCharsets.UTF_8)) {
                tsv.print("address\tsize\tname\tmangled\tsource\n");
                for (Row r : rows) {
                    tsv.printf("0x%016x\t%d\t%s\t%s\t%s\n", r.addr, r.size, r.full, r.mangled, r.source);
                }
            }
        }
        println(String.format("ExportFunctionsCsv: %d functions (%d trimmed due to overlap) -> %s",
            rows.size(), truncated, args[0]));
    }
}
