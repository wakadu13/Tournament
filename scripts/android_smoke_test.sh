#!/bin/bash
# Test de fumee sur emulateur Android : installe l'APK, lance le jeu, tape
# l'adresse du serveur sur le pave numerique, se connecte, tire, et
# enregistre captures d'ecran + journaux.
#
# C'est un VRAI test de bout en bout, pas seulement « l'application
# demarre » : le serveur tourne sur la machine hote de la CI, et le client
# Android lui parle en UDP a travers le NAT de l'emulateur. 10.0.2.2 est
# l'adresse par laquelle un emulateur Android voit la machine qui
# l'heberge. A la fin, on verifie dans le journal DU SERVEUR que le client
# Android s'est bien connecte.
#
# Usage : scripts/android_smoke_test.sh <apk> <dossier de sortie> <journal du serveur>
#
# set -x : affiche chaque commande avant de l'executer (lisible dans la CI).
# Pas de set -e : on veut aller jusqu'au bout pour recuperer captures et
# journaux meme si une etape rate ; les verdicts sont a la fin.
set -x
APK=$1
OUT=$2
SERVER_LOG=$3
PKG=io.github.wakadu13.ematournament
mkdir -p "$OUT"

# root : permet de recuperer la pile des threads du jeu en cas de blocage
adb root || true
adb wait-for-device
adb install -r "$APK"            # -r : remplace une version deja installee
adb logcat -c                    # on ne garde que les logs de ce test
adb shell am start -n $PKG/android.app.NativeActivity
sleep 25                         # demarrage, emulateur lent
adb exec-out screencap -p > "$OUT/android-1-accueil.png"
PID=$(adb shell pidof $PKG | tr -d '\r')
adb shell debuggerd -b "$PID" > "$OUT/android-threads.txt" 2>&1 || true

# Taille de l'ecran. "wm size" repond par ex. "Physical size: 1080x2340",
# en notation portrait : le jeu est en paysage, on echange si besoin.
SIZE=$(adb shell wm size | tail -1 | sed 's/.*: //' | tr -d '\r')
W=${SIZE%x*}; H=${SIZE#*x}
if [ "$W" -lt "$H" ]; then T=$W; W=$H; H=$T; fi
echo "ecran : ${W}x${H}"

# --- Unites d'interface -> pixels -------------------------------------
# Le HUD est dessine sur 720 unites de haut (UI_H dans client/main.c) :
# l'echelle vaut H/720. Une abscisse "a X unites du centre" devient
# W/2 + X*H/720, une ordonnee "a Y unites du haut" devient Y*H/720.
ux() { echo $(( W/2 + $1 * H / 720 )); }
uy() { echo $(( $1 * H / 720 )); }

# Pave numerique de MobileConnectScreen : 3 colonnes de 112 unites
# espacees de 12, a partir de (centre - 180) ; 4 lignes de 78 espacees de
# 12, a partir de y = 236. Centre d'une touche (colonne, ligne) :
#   x = centre - 180 + colonne*124 + 56      y = 236 + ligne*90 + 39
key() {
    adb shell input tap "$(ux $(( -180 + $1 * 124 + 56 )))" "$(uy $(( 236 + $2 * 90 + 39 )))"
    sleep 0.6
}
# Disposition :  1 2 3 / 4 5 6 / 7 8 9 / . 0 :
# "10.0.2.2" = 1(0,0)  0(1,3)  .(0,3)  0(1,3)  .(0,3)  2(1,0)  .(0,3)  2(1,0)
key 0 0; key 1 3; key 0 3; key 1 3; key 0 3; key 1 0; key 0 3; key 1 0
sleep 1
adb exec-out screencap -p > "$OUT/android-2-adresse.png"

# CONNECTER : bouton de 280 x 78 centre en x, a y = 236 + 4*90 + 14 + 39 = 649
adb shell input tap "$(ux 0)" "$(uy 649)"
sleep 12                         # connexion + premieres images de jeu
adb exec-out screencap -p > "$OUT/android-3-jeu.png"

# Appui long sur TIRER : centre a (112, 112) unites du coin bas droit.
# swipe sur place pendant 3000 ms = appui long, en arriere-plan pour
# capturer pendant le tir.
FX=$(( W - 112 * H / 720 )); FY=$(( H - 112 * H / 720 ))
adb shell input swipe $FX $FY $FX $FY 3000 &
sleep 1.5
adb exec-out screencap -p > "$OUT/android-4-tir.png"
wait

# --- Journaux ---------------------------------------------------------
adb logcat -d -s raylib > "$OUT/android-logcat.txt"
adb logcat -d | grep -iE "fatal|crash|ematournament|libmain|ANR" | tail -80 >> "$OUT/android-logcat.txt"
adb logcat -d -b crash > "$OUT/android-crash.txt" 2>&1 || true
cp "$SERVER_LOG" "$OUT/server.log" 2>/dev/null || true
echo "----- logcat (raylib + systeme) -----"; tail -60 "$OUT/android-logcat.txt"
echo "----- logcat (plantages) -----";        tail -60 "$OUT/android-crash.txt"
echo "----- journal du serveur hote -----";   cat "$SERVER_LOG"

# --- Verdicts ---------------------------------------------------------
if ! adb shell pidof $PKG; then echo "ECHEC : le jeu s'est arrete"; exit 1; fi
if grep -qE "Failed to create EGL|Failed to (compile|link)" "$OUT/android-logcat.txt"; then
    echo "ECHEC : erreur graphique"; exit 1; fi
if grep -q "ANR in $PKG" "$OUT/android-logcat.txt"; then
    echo "ECHEC : le jeu ne repond plus (ANR)"; exit 1; fi
# raylib ecrit "Program shader loaded" quand un shader est compile et lie :
# preuve que l'affichage OpenGL ES s'est initialise.
grep -q "Program shader loaded" "$OUT/android-logcat.txt" \
    || { echo "ECHEC : shaders non charges"; exit 1; }
# Le serveur de la machine hote doit avoir vu le client arriver : preuve
# que l'UDP passe de l'APK au serveur, a travers le NAT de l'emulateur.
grep -q "rejoint" "$SERVER_LOG" \
    || { echo "ECHEC : le serveur n'a pas vu le client Android se connecter"; exit 1; }
echo "OK : le client Android a demarre, affiche, et s'est connecte au serveur"
exit 0
