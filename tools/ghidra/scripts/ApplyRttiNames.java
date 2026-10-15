// Applies the names recovered by tools/analysis/rtti.py (rtti.json):
// creates class namespaces, labels typeinfo/vtables and names the virtual
// functions that still have a default name as Class::vfN.
// Usage (headless): -postScript ApplyRttiNames.java <path/rtti.json>
// @category PLARecomp

import java.io.FileReader;
import java.io.Reader;
import java.util.HashSet;
import java.util.Set;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.app.util.SymbolPath;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolUtilities;

public class ApplyRttiNames extends GhidraScript {

    private SymbolTable st;
    private int namedFuncs = 0, createdFuncs = 0, classes = 0, failures = 0;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("Usage: ApplyRttiNames.java <rtti.json>");
            return;
        }
        st = currentProgram.getSymbolTable();
        JsonObject root;
        try (Reader r = new FileReader(args[0], java.nio.charset.StandardCharsets.UTF_8)) {
            root = JsonParser.parseReader(r).getAsJsonObject();
        }
        JsonObject cls = root.getAsJsonObject("classes");
        // Order: bases before derived classes, so that an inherited function that is not
        // overridden keeps the name of the class that introduced it.
        Set<String> done = new HashSet<>();
        for (String name : cls.keySet()) {
            if (monitor.isCancelled()) {
                break;
            }
            visit(name, cls, done);
        }
        JsonArray anon = root.has("vtables_without_rtti") ? root.getAsJsonArray("vtables_without_rtti") : new JsonArray();
        for (JsonElement e : anon) {
            JsonObject v = e.getAsJsonObject();
            String name = v.get("name").getAsString();
            Namespace ns = classNamespace(name);
            if (ns == null) {
                continue;
            }
            label(v.get("vtable").getAsString(), "vtable", ns);
            nameVfuncs(v.getAsJsonArray("vtables"), ns);
        }
        println(String.format("ApplyRttiNames: %d classes, %d functions named, %d functions created, %d failures",
            classes, namedFuncs, createdFuncs, failures));
    }

    private void visit(String name, JsonObject cls, Set<String> done) throws Exception {
        if (done.contains(name) || !cls.has(name)) {
            return;
        }
        done.add(name);
        JsonObject c = cls.getAsJsonObject(name);
        for (JsonElement b : c.getAsJsonArray("bases")) {
            visit(b.getAsJsonObject().get("name").getAsString(), cls, done);
        }
        Namespace ns = classNamespace(name);
        if (ns == null) {
            return;
        }
        classes++;
        label(c.get("typeinfo").getAsString(), "typeinfo", ns);
        label(c.get("typeinfo_name").getAsString(), "typeinfo_name", ns);
        if (c.has("vtable")) {
            label(c.get("vtable").getAsString(), "vtable", ns);
            nameVfuncs(c.getAsJsonArray("vtables"), ns);
        }
    }

    private Namespace classNamespace(String name) {
        try {
            // "@<addr>" disambiguates same-named classes; Ghidra does not allow it in namespaces.
            String clean = name.replace("@", "_at_");
            SymbolPath path = new SymbolPath(clean);
            // The hierarchy is created component by component, sanitizing each name
            // (templates may bring unsupported spaces or parentheses).
            Namespace parent = currentProgram.getGlobalNamespace();
            java.util.List<String> parts = path.asList();
            for (int i = 0; i < parts.size() - 1; i++) {
                String comp = SymbolUtilities.replaceInvalidChars(parts.get(i), true);
                Namespace next = NamespaceUtils.getFirstNonFunctionNamespace(parent, comp, currentProgram);
                if (next == null) {
                    next = st.createNameSpace(parent, comp, SourceType.ANALYSIS);
                }
                parent = next;
            }
            String leaf = SymbolUtilities.replaceInvalidChars(path.getName(), true);
            Namespace ns = NamespaceUtils.getFirstNonFunctionNamespace(parent, leaf, currentProgram);
            if (ns == null) {
                ns = st.createClass(parent, leaf, SourceType.ANALYSIS);
            }
            else if (!(ns instanceof GhidraClass)) {
                ns = NamespaceUtils.convertNamespaceToClass(ns);
            }
            return ns;
        }
        catch (Exception e) {
            failures++;
            if (failures < 20) {
                printerr("namespace " + name + ": " + e.getMessage());
            }
            return null;
        }
    }

    private void label(String addr, String name, Namespace ns) {
        try {
            Address a = toAddr(Long.parseUnsignedLong(addr.substring(2), 16));
            for (Symbol s : st.getSymbols(a)) {
                if (s.getName().equals(name) && s.getParentNamespace().equals(ns)) {
                    return;
                }
            }
            st.createLabel(a, name, ns, SourceType.ANALYSIS);
        }
        catch (Exception e) {
            failures++;
        }
    }

    private void nameVfuncs(JsonArray vtables, Namespace ns) {
        for (JsonElement ve : vtables) {
            JsonObject sv = ve.getAsJsonObject();
            long ott = sv.get("offset_to_top").getAsLong();
            JsonArray entries = sv.getAsJsonArray("entries");
            for (int i = 0; i < entries.size(); i++) {
                JsonElement e = entries.get(i);
                if (e.isJsonNull()) {
                    continue;
                }
                String s = e.getAsString();
                if (!s.startsWith("0x")) {
                    continue; // import (e.g. __cxa_pure_virtual)
                }
                try {
                    Address a = toAddr(Long.parseUnsignedLong(s.substring(2), 16));
                    Function f = getFunctionAt(a);
                    if (f == null) {
                        f = createFunction(a, null);
                        if (f == null) {
                            continue;
                        }
                        createdFuncs++;
                    }
                    if (f.getSymbol().getSource() != SourceType.DEFAULT) {
                        continue; // already has a name (symbol, demangler, another class...)
                    }
                    String n = ott == 0 ? "vf" + i : "thunk" + (-ott) + "_vf" + i;
                    f.setName(n, SourceType.ANALYSIS);
                    f.setParentNamespace(ns);
                    namedFuncs++;
                }
                catch (Exception ex) {
                    failures++;
                }
            }
        }
    }
}
