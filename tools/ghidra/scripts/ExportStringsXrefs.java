// Exports defined strings and imports with their references from code.
//   <out>/strings.tsv  address  len  nrefs  functions_using_it  string(escaped)
//   <out>/imports.tsv  name     nrefs  functions_calling_it
// Function lists are limited to 64 entries per row.
// Usage: -postScript ExportStringsXrefs.java <output_directory>
// @category PLARecomp

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.util.DefinedStringIterator;

public class ExportStringsXrefs extends GhidraScript {

    private static final int MAX_LIST = 64;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : ".");
        out.mkdirs();
        int nstr = 0, nimp = 0;
        try (PrintWriter w = new PrintWriter(new File(out, "strings.tsv"), StandardCharsets.UTF_8)) {
            w.print("address\tlen\tnrefs\tfunctions\tstring\n");
            for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
                if (monitor.isCancelled()) {
                    break;
                }
                StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
                String s = sdi.getStringValue();
                if (s == null) {
                    continue;
                }
                Set<String> funcs = new LinkedHashSet<>();
                int nrefs = 0;
                ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
                while (refs.hasNext()) {
                    Reference r = refs.next();
                    nrefs++;
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null && funcs.size() < MAX_LIST) {
                        funcs.add("0x" + Long.toHexString(f.getEntryPoint().getOffset()));
                    }
                }
                w.printf("0x%x\t%d\t%d\t%s\t%s\n", d.getAddress().getOffset(), d.getLength(), nrefs,
                    String.join(",", funcs), escape(s));
                nstr++;
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "imports.tsv"), StandardCharsets.UTF_8)) {
            w.print("name\tnrefs\tfunctions\n");
            FunctionIterator it = currentProgram.getFunctionManager().getExternalFunctions();
            while (it.hasNext() && !monitor.isCancelled()) {
                Function ext = it.next();
                Set<String> funcs = new LinkedHashSet<>();
                int nrefs = 0;
                Address[] thunks = ext.getFunctionThunkAddresses(true);
                if (thunks != null) {
                    for (Address t : thunks) {
                        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(t);
                        while (refs.hasNext()) {
                            Reference r = refs.next();
                            nrefs++;
                            Function f = getFunctionContaining(r.getFromAddress());
                            if (f != null && funcs.size() < MAX_LIST) {
                                funcs.add("0x" + Long.toHexString(f.getEntryPoint().getOffset()));
                            }
                        }
                    }
                }
                w.printf("%s\t%d\t%s\n", ext.getName(true), nrefs, String.join(",", funcs));
                nimp++;
            }
        }
        println(String.format("ExportStringsXrefs: %d strings, %d imports -> %s", nstr, nimp, out));
    }

    private static String escape(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toCharArray()) {
            switch (c) {
                case '\t': b.append("\\t"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\\': b.append("\\\\"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\x%02x", (int) c));
                    }
                    else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }
}
