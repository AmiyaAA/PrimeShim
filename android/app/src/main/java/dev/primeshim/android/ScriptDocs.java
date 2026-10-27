package dev.primeshim.android;

import java.util.ArrayList;
import java.util.List;

/** Localized reference entries for the script help browser. */
public final class ScriptDocs {
    public static final class Entry {
        public final String name;
        public final String signature;
        public final String description;
        public final String parameters;
        public final String returns;
        public final String example;

        Entry(String name, String signature, String description,
              String parameters, String returns, String example) {
            this.name = name;
            this.signature = signature;
            this.description = description;
            this.parameters = parameters;
            this.returns = returns;
            this.example = example;
        }
    }

    public static List<Entry> forLanguage(String lang) {
        if ("en".equals(lang)) return english();
        if ("fr".equals(lang)) return french();
        return chinese();
    }

    private static List<Entry> chinese() {
        List<Entry> out = new ArrayList<>();
        out.add(new Entry("prime.press", "await prime.press(name)",
                "按下并松开一个固件按键。默认等待 60ms，适合输入单个字符。",
                "name：键名或键码，例如 \"1\"、\"enter\"、\"shift\"。",
                "Promise，完成后继续执行下一行。",
                "await prime.press(\"1\");\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.key", "prime.key(name, down)",
                "直接发送按键按下或松开事件，不自动松开。适合组合键和长按。",
                "name：键名或键码。\ndown：true 按下，false 松开。",
                "无返回值。",
                "prime.key(\"shift\", true);\nawait prime.sleep(100);\nprime.key(\"shift\", false);"));
        out.add(new Entry("prime.keyDown", "prime.keyDown(name)",
                "按住一个键，等同于 prime.key(name, true)。",
                "name：键名或键码。",
                "无返回值。",
                "prime.keyDown(\"alpha\");\nawait prime.press(\"1\");\nprime.keyUp(\"alpha\");"));
        out.add(new Entry("prime.keyUp", "prime.keyUp(name)",
                "松开一个键，等同于 prime.key(name, false)。",
                "name：键名或键码。",
                "无返回值。",
                "prime.keyUp(\"shift\");"));
        out.add(new Entry("prime.touchDown", "prime.touchDown(id, x, y)",
                "在固件坐标上按下触摸点。id 用于区分多指。",
                "id：0..7 的触摸点编号。\nx：0..319。\ny：0..239。",
                "无返回值。",
                "prime.touchDown(0, 160, 120);"));
        out.add(new Entry("prime.touchMove", "prime.touchMove(id, x, y)",
                "移动已经按下的触摸点。",
                "id：0..7 的触摸点编号。\nx：0..319。\ny：0..239。",
                "无返回值。",
                "prime.touchMove(0, 180, 130);"));
        out.add(new Entry("prime.touchUp", "prime.touchUp(id, x, y)",
                "抬起触摸点。通常使用最后的位置。",
                "id：0..7 的触摸点编号。\nx：0..319。\ny：0..239。",
                "无返回值。",
                "prime.touchUp(0, 180, 130);"));
        out.add(new Entry("prime.touchTap", "await prime.touchTap(id, x, y)",
                "按下并抬起一个触摸点，默认间隔 60ms。",
                "id：0..7 的触摸点编号。\nx：0..319。\ny：0..239。",
                "Promise，完成后继续执行下一行。",
                "await prime.touchTap(0, 160, 120);"));
        out.add(new Entry("prime.sleep", "await prime.sleep(ms)",
                "等待指定毫秒数。必须使用 await 才能暂停脚本。",
                "ms：毫秒，最小为 0。",
                "Promise。",
                "await prime.sleep(500);\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.log", "prime.log(...)",
                "把参数写入脚本日志，不影响固件。",
                "任意数量的值，会用空格连接。",
                "无返回值。",
                "prime.log(\"done\", 42);"));
        out.add(new Entry("prime.keys", "prime.keys()",
                "返回当前可用的按键名 JSON 数组，可用于调试键名。",
                "无参数。",
                "JSON 字符串。",
                "prime.log(prime.keys());"));
        return out;
    }

    private static List<Entry> english() {
        List<Entry> out = new ArrayList<>();
        out.add(new Entry("prime.press", "await prime.press(name)",
                "Press and release a firmware key. The default gap is 60 ms, suitable for typing a single character.",
                "name: a key name or code, such as \"1\", \"enter\" or \"shift\".",
                "A Promise; await it before continuing.",
                "await prime.press(\"1\");\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.key", "prime.key(name, down)",
                "Send a raw key-down or key-up event without releasing automatically. Useful for modifiers and holds.",
                "name: a key name or code.\ndown: true for down, false for up.",
                "No return value.",
                "prime.key(\"shift\", true);\nawait prime.sleep(100);\nprime.key(\"shift\", false);"));
        out.add(new Entry("prime.keyDown", "prime.keyDown(name)",
                "Hold a key; equivalent to prime.key(name, true).",
                "name: a key name or code.",
                "No return value.",
                "prime.keyDown(\"alpha\");\nawait prime.press(\"1\");\nprime.keyUp(\"alpha\");"));
        out.add(new Entry("prime.keyUp", "prime.keyUp(name)",
                "Release a key; equivalent to prime.key(name, false).",
                "name: a key name or code.",
                "No return value.",
                "prime.keyUp(\"shift\");"));
        out.add(new Entry("prime.touchDown", "prime.touchDown(id, x, y)",
                "Press a touch point in firmware coordinates. Use id for multi-touch.",
                "id: touch id 0..7.\nx: 0..319.\ny: 0..239.",
                "No return value.",
                "prime.touchDown(0, 160, 120);"));
        out.add(new Entry("prime.touchMove", "prime.touchMove(id, x, y)",
                "Move a touch point that is already down.",
                "id: touch id 0..7.\nx: 0..319.\ny: 0..239.",
                "No return value.",
                "prime.touchMove(0, 180, 130);"));
        out.add(new Entry("prime.touchUp", "prime.touchUp(id, x, y)",
                "Lift a touch point. Usually use its last position.",
                "id: touch id 0..7.\nx: 0..319.\ny: 0..239.",
                "No return value.",
                "prime.touchUp(0, 180, 130);"));
        out.add(new Entry("prime.touchTap", "await prime.touchTap(id, x, y)",
                "Press and lift a touch point with a 60 ms default gap.",
                "id: touch id 0..7.\nx: 0..319.\ny: 0..239.",
                "A Promise; await it before continuing.",
                "await prime.touchTap(0, 160, 120);"));
        out.add(new Entry("prime.sleep", "await prime.sleep(ms)",
                "Wait for the given number of milliseconds. Use await to actually pause.",
                "ms: milliseconds; values below zero become zero.",
                "A Promise.",
                "await prime.sleep(500);\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.log", "prime.log(...)",
                "Append the arguments to the script log without affecting the firmware.",
                "Any number of values; they are joined with spaces.",
                "No return value.",
                "prime.log(\"done\", 42);"));
        out.add(new Entry("prime.keys", "prime.keys()",
                "Return the available key names as a JSON array for debugging.",
                "No parameters.",
                "A JSON string.",
                "prime.log(prime.keys());"));
        return out;
    }

    private static List<Entry> french() {
        List<Entry> out = new ArrayList<>();
        out.add(new Entry("prime.press", "await prime.press(name)",
                "Appuie puis relâche une touche du firmware. L'intervalle par défaut est de 60 ms, adapté à un caractère.",
                "name : nom ou code de touche, par exemple \"1\", \"enter\" ou \"shift\".",
                "Une Promise ; utilisez await avant de continuer.",
                "await prime.press(\"1\");\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.key", "prime.key(name, down)",
                "Envoie un appui ou un relâchement brut, sans relâchement automatique. Utile pour les modificateurs.",
                "name : nom ou code de touche.\ndown : true pour appuyer, false pour relâcher.",
                "Aucune valeur de retour.",
                "prime.key(\"shift\", true);\nawait prime.sleep(100);\nprime.key(\"shift\", false);"));
        out.add(new Entry("prime.keyDown", "prime.keyDown(name)",
                "Maintient une touche ; équivaut à prime.key(name, true).",
                "name : nom ou code de touche.",
                "Aucune valeur de retour.",
                "prime.keyDown(\"alpha\");\nawait prime.press(\"1\");\nprime.keyUp(\"alpha\");"));
        out.add(new Entry("prime.keyUp", "prime.keyUp(name)",
                "Relâche une touche ; équivaut à prime.key(name, false).",
                "name : nom ou code de touche.",
                "Aucune valeur de retour.",
                "prime.keyUp(\"shift\");"));
        out.add(new Entry("prime.touchDown", "prime.touchDown(id, x, y)",
                "Appuie sur un point tactile en coordonnées firmware. id distingue les doigts.",
                "id : identifiant 0..7.\nx : 0..319.\ny : 0..239.",
                "Aucune valeur de retour.",
                "prime.touchDown(0, 160, 120);"));
        out.add(new Entry("prime.touchMove", "prime.touchMove(id, x, y)",
                "Déplace un point tactile déjà appuyé.",
                "id : identifiant 0..7.\nx : 0..319.\ny : 0..239.",
                "Aucune valeur de retour.",
                "prime.touchMove(0, 180, 130);"));
        out.add(new Entry("prime.touchUp", "prime.touchUp(id, x, y)",
                "Relève un point tactile. Utilisez en général sa dernière position.",
                "id : identifiant 0..7.\nx : 0..319.\ny : 0..239.",
                "Aucune valeur de retour.",
                "prime.touchUp(0, 180, 130);"));
        out.add(new Entry("prime.touchTap", "await prime.touchTap(id, x, y)",
                "Appuie et relève un point tactile, avec 60 ms d'intervalle par défaut.",
                "id : identifiant 0..7.\nx : 0..319.\ny : 0..239.",
                "Une Promise ; utilisez await avant de continuer.",
                "await prime.touchTap(0, 160, 120);"));
        out.add(new Entry("prime.sleep", "await prime.sleep(ms)",
                "Attend le nombre de millisecondes demandé. Utilisez await pour suspendre réellement.",
                "ms : millisecondes ; les valeurs négatives valent zéro.",
                "Une Promise.",
                "await prime.sleep(500);\nawait prime.press(\"enter\");"));
        out.add(new Entry("prime.log", "prime.log(...)",
                "Ajoute les arguments au journal du script sans affecter le firmware.",
                "Un nombre quelconque de valeurs, jointes par des espaces.",
                "Aucune valeur de retour.",
                "prime.log(\"done\", 42);"));
        out.add(new Entry("prime.keys", "prime.keys()",
                "Renvoie les noms de touches disponibles dans un tableau JSON.",
                "Aucun paramètre.",
                "Une chaîne JSON.",
                "prime.log(prime.keys());"));
        return out;
    }

    private ScriptDocs() {}
}
