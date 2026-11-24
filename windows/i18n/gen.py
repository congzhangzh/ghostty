#!/usr/bin/env python3
"""windows/i18n/gen.py: the host's translation table (windows/i18n_table.h), zh-CN, zh-TW, ja, fr.

Run from the repo root: python3 windows/i18n/gen.py. Two sources:
- the command palette's titles and descriptions (src/input/command.zig, i18n.N_) with Ghostty's
  own, human translations from po/<lang>.po (upstream's; refreshed by running this again);
- the Windows host's own strings (menus, hints, dialogs) and the palette strings po/ lacks, from
  HOST below: MACHINE-MADE translations, marked so in the table, for the owner's review.
English is the fallback, and the motto stays English everywhere (as in pys).
"""
import re, sys
from pathlib import Path

LANGS = ("zh_CN", "zh_TW", "ja", "fr")

# English → (zh_CN, zh_TW, ja, fr). Machine-made: review before trusting.
HOST = {
    "About Ghostty for Windows…": ("关于 Ghostty for Windows…", "關於 Ghostty for Windows…", "Ghostty for Windows について…", "À propos de Ghostty for Windows…"),
    "What's new…": ("新功能…", "新功能…", "新機能…", "Nouveautés…"),
    "Open config…": ("打开配置…", "開啟設定…", "設定を開く…", "Ouvrir la configuration…"),
    "Reload config": ("重新加载配置", "重新載入設定", "設定を再読み込み", "Recharger la configuration"),
    "Theme": ("主题", "主題", "テーマ", "Thème"),
    "Default (from my config)": ("默认（来自我的配置）", "預設（來自我的設定）", "既定（自分の設定から）", "Par défaut (ma configuration)"),
    # Font ▸ and Shell ▸ (font.c, shell.c).
    "Font": ("字体", "字型", "フォント", "Police"),
    "More fonts…": ("更多字体…", "更多字型…", "その他のフォント…", "Plus de polices…"),
    "Chinese / Japanese": ("中文 / 日文", "中文 / 日文", "中国語 / 日本語", "Chinois / japonais"),
    "Automatic (by display language)": ("自动（按显示语言）", "自動（依顯示語言）", "自動（表示言語による）", "Automatique (selon la langue d'affichage)"),
    "Size": ("字号", "字級", "サイズ", "Taille"),
    "Reset size": ("重置字号", "重設字級", "サイズをリセット", "Réinitialiser la taille"),
    "Shell": ("Shell", "Shell", "シェル", "Shell"),
    "For new tabs and windows": ("用于新的标签页和窗口", "用於新的分頁和視窗", "新しいタブとウィンドウ用", "Pour les nouveaux onglets et fenêtres"),
    "Windows PowerShell": ("Windows PowerShell", "Windows PowerShell", "Windows PowerShell", "Windows PowerShell"),
    "Command Prompt": ("命令提示符", "命令提示字元", "コマンド プロンプト", "Invite de commandes"),
    "More themes…": ("更多主题…", "更多主題…", "その他のテーマ…", "Plus de thèmes…"),
    "Auto (follow Windows)": ("自动（跟随 Windows）", "自動（跟隨 Windows）", "自動（Windows に合わせる）", "Auto (suivre Windows)"),
    "Auto": ("自动", "自動", "自動", "Auto"),
    "Light": ("浅色", "淺色", "ライト", "Clair"),
    "Dark": ("深色", "深色", "ダーク", "Sombre"),
    "Type a command, or a terminal's title": ("输入命令或终端标题", "輸入命令或終端標題", "コマンドまたはターミナルのタイトルを入力", "Tapez une commande ou le titre d'un terminal"),
    "Focus: ": ("切换到：", "切換到：", "移動: ", "Aller à : "),
    "Jump to this terminal": ("跳转到此终端", "跳到此終端", "このターミナルへ移動", "Aller à ce terminal"),
    "Untitled": ("无标题", "無標題", "無題", "Sans titre"),
    "Exit full screen": ("退出全屏", "結束全螢幕", "全画面を終了", "Quitter le plein écran"),
    "Back to the window": ("回到窗口", "回到視窗", "ウィンドウに戻る", "Revenir à la fenêtre"),
    "Move the mouse to the top for tabs and ?, F11 to exit": ("将鼠标移到顶部可显示标签页和 ?，按 F11 退出", "將滑鼠移到頂端可顯示分頁與 ?，按 F11 結束", "上端にマウスを移動するとタブと ? が出ます。F11 で終了", "Placez la souris en haut pour les onglets et ?, F11 pour quitter"),
    "Quick Terminal key %ls is used by another app": ("快速终端的按键 %ls 已被其他程序占用", "快速終端的按鍵 %ls 已被其他程式佔用", "クイックターミナルのキー %ls は別のアプリが使用中です", "La touche %ls du terminal rapide est utilisée par une autre application"),
    "Quick Terminal key %ls is used by Windows Terminal (its quake mode)": ("快速终端的按键 %ls 已被 Windows Terminal 占用（它的 quake 模式）", "快速終端的按鍵 %ls 已被 Windows Terminal 佔用（它的 quake 模式）", "クイックターミナルのキー %ls は Windows Terminal が使用中です（その quake モード）", "La touche %ls du terminal rapide est utilisée par Windows Terminal (son mode quake)"),
    "%ls: this theme has no light/dark pair (Theme menu: pick a pair)": ("%ls：此主题没有浅色/深色配对（在主题菜单中选择一对）", "%ls：此主題沒有淺色/深色配對（在主題選單中選擇一對）", "%ls: このテーマにはライト/ダークの組がありません（テーマメニューで組を選んでください）", "%ls : ce thème n'a pas de paire clair/sombre (menu Thème : choisissez une paire)"),
    "The text you are pasting may run commands. Paste it?": ("要粘贴的文本可能会执行命令。仍要粘贴吗？", "要貼上的文字可能會執行命令。仍要貼上嗎？", "貼り付けるテキストはコマンドを実行する可能性があります。貼り付けますか？", "Le texte que vous collez peut exécuter des commandes. Le coller ?"),
    "A program in the terminal wants to read the clipboard. Allow it?": ("终端中的程序想要读取剪贴板。允许吗？", "終端中的程式想要讀取剪貼簿。允許嗎？", "ターミナル内のプログラムがクリップボードを読み取ろうとしています。許可しますか？", "Un programme du terminal veut lire le presse-papiers. L'autoriser ?"),
    "A program in the terminal wants to set the clipboard. Allow it?": ("终端中的程序想要写入剪贴板。允许吗？", "終端中的程式想要寫入剪貼簿。允許嗎？", "ターミナル内のプログラムがクリップボードに書き込もうとしています。許可しますか？", "Un programme du terminal veut modifier le presse-papiers. L'autoriser ?"),
    "Overview: every command and its key": ("总览：所有命令及其按键", "總覽：所有命令及其按鍵", "一覧: すべてのコマンドとそのキー", "Vue d'ensemble : toutes les commandes et leurs touches"),
    # The cheat sheet (sheet.c): groups and the host's own keys.
    "Tabs": ("标签页", "分頁", "タブ", "Onglets"),
    "Splits": ("分屏", "分割", "分割", "Divisions"),
    "Window & full screen": ("窗口与全屏", "視窗與全螢幕", "ウィンドウと全画面", "Fenêtre et plein écran"),
    "Clipboard & links": ("剪贴板与链接", "剪貼簿與連結", "クリップボードとリンク", "Presse-papiers et liens"),
    "Themes & config": ("主题与配置", "主題與設定", "テーマと設定", "Thèmes et configuration"),
    "Quick Terminal": ("快速终端", "快速終端", "クイックターミナル", "Terminal rapide"),
    "Help": ("帮助", "說明", "ヘルプ", "Aide"),
    "Window menu": ("窗口菜单", "視窗選單", "ウィンドウメニュー", "Menu de la fenêtre"),
    "Open a link": ("打开链接", "開啟連結", "リンクを開く", "Ouvrir un lien"),
    "Type a file's path": ("输入文件路径", "輸入檔案路徑", "ファイルのパスを入力", "Taper le chemin d'un fichier"),
    "Drag & drop": ("拖放", "拖放", "ドラッグ＆ドロップ", "Glisser-déposer"),
    "Light / Dark / Auto": ("浅色 / 深色 / 自动", "淺色 / 深色 / 自動", "ライト / ダーク / 自動", "Clair / Sombre / Auto"),
    "Theme list": ("主题列表", "主題列表", "テーマ一覧", "Liste des thèmes"),
    "This sheet": ("本速查表", "本速查表", "この一覧", "Cette fiche"),
    "Stats overlay": ("统计浮层", "統計浮層", "統計オーバーレイ", "Statistiques"),
    "Esc, ? or a click: close": ("Esc、? 或点击：关闭", "Esc、? 或點擊：關閉", "Esc・?・クリック: 閉じる", "Échap, ? ou un clic : fermer"),
    "Next Tab": ("下一个标签页", "下一個分頁", "次のタブ", "Onglet suivant"),
    "Previous Tab": ("上一个标签页", "上一個分頁", "前のタブ", "Onglet précédent"),
    "Command Palette": ("命令面板", "命令面板", "コマンドパレット", "Palette de commandes"),
    "Open Config": ("打开配置", "開啟設定", "設定を開く", "Ouvrir la configuration"),
    "Show or hide": ("显示或隐藏", "顯示或隱藏", "表示・非表示", "Afficher ou masquer"),
    "Go to tab 1…9": ("跳到第 1…9 个标签页", "跳到第 1…9 個分頁", "タブ 1…9 へ移動", "Aller à l'onglet 1…9"),
    # The palette strings po/ lacks (upstream added them after its last translation round).
    "Prompt for a new title for the current window.": ("为当前窗口输入新标题。", "為目前視窗輸入新標題。", "現在のウィンドウの新しいタイトルを入力します。", "Saisir un nouveau titre pour la fenêtre actuelle."),
    "Switch to Configured Appearance": ("切换到配置的外观", "切換到設定的外觀", "設定の外観に切り替え", "Passer à l'apparence configurée"),
    "Use the application appearance from the configuration.": ("使用配置中的应用外观。", "使用設定中的應用程式外觀。", "設定にあるアプリの外観を使います。", "Utiliser l'apparence de l'application définie dans la configuration."),
    "Switch to System Appearance": ("切换到系统外观", "切換到系統外觀", "システムの外観に切り替え", "Passer à l'apparence du système"),
    "Use the system application appearance for this session.": ("本次会话使用系统的应用外观。", "本次工作階段使用系統的應用程式外觀。", "このセッションではシステムのアプリ外観を使います。", "Utiliser l'apparence du système pour cette session."),
    "Switch to Light Appearance": ("切换到浅色外观", "切換到淺色外觀", "ライトの外観に切り替え", "Passer à l'apparence claire"),
    "Use a light application appearance for this session.": ("本次会话使用浅色外观。", "本次工作階段使用淺色外觀。", "このセッションではライトの外観を使います。", "Utiliser une apparence claire pour cette session."),
    "Switch to Dark Appearance": ("切换到深色外观", "切換到深色外觀", "ダークの外観に切り替え", "Passer à l'apparence sombre"),
    "Use a dark application appearance for this session.": ("本次会话使用深色外观。", "本次工作階段使用深色外觀。", "このセッションではダークの外観を使います。", "Utiliser une apparence sombre pour cette session."),
}

def po(path):
    s = path.read_text(encoding="utf-8"); d = {}
    for m in re.finditer(r'msgid ((?:"(?:[^"\\]|\\.)*"\s*)+)msgstr ((?:"(?:[^"\\]|\\.)*"\s*)+)', s):
        k = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        v = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
        if k and v: d[k] = v
    return d

def c(s):  # a C wide literal of s (already C-escaped from po/zig, or plain)
    return 'L"' + s.replace('"', '\\"') + '"' if '\\"' not in s else 'L"' + s + '"'

def main():
    root = Path(__file__).resolve().parents[2]
    cmd = (root / "src/input/command.zig").read_text(encoding="utf-8")
    palette = []
    for k, t in re.findall(r'\.(title|description)\s*=\s*(?:i18n\.N_\()?"((?:[^"\\]|\\.)*)"', cmd):
        if t not in palette: palette.append(t)
    tables = {lang: po(root / f"po/{lang}.po") for lang in LANGS}
    rows, upstream, machine = [], 0, 0
    for en in palette + [k for k in HOST if k not in palette]:
        if all(en in tables[l] for l in LANGS):
            rows.append((en, [tables[l][en] for l in LANGS], "upstream po"))
            upstream += 1
        elif en in HOST:
            rows.append((en, list(HOST[en]), "MACHINE"))
            machine += 1
    out = ["// Generated by windows/i18n/gen.py: do not edit; edit gen.py (or po/) and run it again.",
           f"// {upstream} strings with Ghostty's own translations (po/), {machine} MACHINE-MADE (review them).",
           "// Columns: English, zh-CN, zh-TW, ja, fr.", ""]
    for en, t, src in rows:
        out.append(f"{{{c(en)}, {{{', '.join(c(x) for x in t)}}}}},  // {src}")
    (root / "windows/i18n_table.h").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"windows/i18n_table.h: {upstream} upstream, {machine} machine-made")

main()
