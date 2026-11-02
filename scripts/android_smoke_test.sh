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

# L'emulateur demarre en PORTRAIT. Au lancement, le systeme applique
# l'orientation paysage du manifeste et RECREE l'activite : raylib
# s'initialise deux fois, et la file d'entrees ne se rattache pas toujours
# au second looper (ecran vivant, aucun contact percu — vu en CI). En
# mettant l'emulateur en paysage AVANT de lancer, il n'y a plus de
# rotation, donc une seule initialisation.
adb shell settings put system accelerometer_rotation 0
adb shell settings put system user_rotation 1      # 1 = paysage (90 degres)
sleep 2

# Lance le jeu et attend l'ecran d'accueil. Rend le PID.
launch_game() {
    adb logcat -c                # on ne garde que les logs de cette tentative
    adb shell am start -n $PKG/android.app.NativeActivity
    sleep 25                     # demarrage, emulateur lent
    PID=$(adb shell pidof $PKG | tr -d '\r')
    echo "initialisations de raylib : $(adb logcat -d -s raylib | grep -c 'Initializing raylib')"
}
launch_game
adb exec-out screencap -p > "$OUT/android-1-accueil.png"
adb shell debuggerd -b "$PID" > "$OUT/android-threads.txt" 2>&1 || true

# --- Calibration : du pixel injecte au pixel que le jeu recoit ------------
# Entre « input swipe » et ce que raylib rapporte au jeu s'intercalent la
# barre de navigation, l'orientation forcee, d'eventuelles marges : le
# decalage n'est pas previsible (vu en CI : -204 px en X). On ne calcule
# donc RIEN a partir d'une taille d'ecran. On appuie sur deux points connus
# d'une zone vide (le bandeau du haut), on relit dans logcat ou le jeu les a
# recus, et on en deduit decalage et echelle. Toutes les cibles sont ensuite
# exprimees dans le repere DU JEU, puis converties.
PHYS=$(adb shell wm size | tail -1 | sed 's/.*: //' | tr -d '\r')
PW=${PHYS%x*}; PH=${PHYS#*x}
if [ "$PW" -lt "$PH" ]; then T=$PW; PW=$PH; PH=$T; fi
echo "ecran physique : ${PW}x${PH}"

# Un appui qui DURE (voir plus bas) : 180 ms, plusieurs images meme a 24 i/s
press() { adb shell input swipe "$1" "$2" "$1" "$2" 180; }

# Dernier appui recu par le jeu : "appui relache en (X, Y) px"
last_press() {
    adb logcat -d -s raylib | grep -o "appui relache en ([0-9]*, [0-9]*)" | tail -1 | tr -dc '0-9 ,'
}
CX1=300; CX2=$(( PW - 300 )); CY=60
press $CX1 $CY; sleep 0.8
P1=$(last_press)
if [ -z "$P1" ]; then
    echo "AUCUN appui percu par le jeu : relance de l'application (tentative 2)"
    adb shell am force-stop $PKG
    sleep 2
    launch_game
    press $CX1 $CY; sleep 0.8
    P1=$(last_press)
    [ -n "$P1" ] || { echo "ECHEC : le jeu ne percoit aucun contact"; exit 1; }
fi
press $CX2 $CY; sleep 0.8
P2=$(last_press)
echo "calibration : ($CX1,$CY) -> ($P1)  ($CX2,$CY) -> ($P2)"
G1X=${P1%,*}; G1Y=${P1#*, }; G2X=${P2%,*}
# echelle et decalage (arithmetique entiere en millieme)
SCALE=$(( (G2X - G1X) * 1000 / (CX2 - CX1) ))      # x1000
OX=$(( G1X - CX1 * SCALE / 1000 ))
OY=$(( G1Y - CY * SCALE / 1000 ))
echo "echelle x1000 = $SCALE, decalage = ($OX, $OY)"
[ "$SCALE" -gt 500 ] && [ "$SCALE" -lt 2000 ] || { echo "ECHEC : calibration aberrante"; exit 1; }
# repere du jeu -> pixel physique a injecter
phys_x() { echo $(( ($1 - OX) * 1000 / SCALE )); }
phys_y() { echo $(( ($1 - OY) * 1000 / SCALE )); }

# --- Geometrie du pave, dans le repere du jeu ---------------------------
# Le jeu journalise sa taille d'ecran (« ecran=WxH ») : l'echelle de
# l'interface vaut H/720 (UI_H dans client/main.c), la largeur virtuelle
# W/echelle. Une abscisse "a X unites du centre" devient W/2 + X*H/720,
# une ordonnee "a Y unites du haut" devient Y*H/720 — en pixels du JEU.
GS=$(adb logcat -d -s raylib | grep -o "ecran=[0-9]*x[0-9]*" | tail -1 | tr -dc '0-9x')
W=${GS%x*}; H=${GS#*x}
echo "ecran vu par le jeu : ${W}x${H}"
ux() { phys_x $(( W/2 + $1 * H / 720 )); }
uy() { phys_y $(( $1 * H / 720 )); }

# Pave numerique de MobileConnectScreen : 3 colonnes de 112 unites
# espacees de 12, a partir de (centre - 180) ; 4 lignes de 78 espacees de
# 12, a partir de y = 236. Centre d'une touche (colonne, ligne) :
#   x = centre - 180 + colonne*124 + 56      y = 236 + ligne*90 + 39
key() {
    press "$(ux $(( -180 + $1 * 124 + 56 )))" "$(uy $(( 236 + $2 * 90 + 39 )))"
    sleep 0.5
}
# Effacement : bouton a droite du champ (champ : centre-200..centre+200, y 150..212 ;
# bouton : +12 apres le champ, 76 de large) -> centre a (centre+250, 181)
erase() { press "$(ux 250)" "$(uy 181)"; sleep 0.4; }
# Disposition :  1 2 3 / 4 5 6 / 7 8 9 / . 0 :
# "10.0.2.2" = 1(0,0)  0(1,3)  .(0,3)  0(1,3)  .(0,3)  2(1,0)  .(0,3)  2(1,0)
type_address() { key 0 0; key 1 3; key 0 3; key 1 3; key 0 3; key 1 0; key 0 3; key 1 0; }
typed() { adb logcat -d -s raylib | grep -o 'adresse "[^"]*"' | tail -1 | tr -d '"' | sed 's/adresse //'; }

type_address
sleep 1
# Le jeu journalise l'adresse apres chaque touche : on la relit. Si elle
# n'est pas la bonne (appui perdu ou mal place), on efface et on retape,
# deux fois au plus.
for attempt in 1 2; do
    [ "$(typed)" = "10.0.2.2" ] && break
    echo "adresse composee « $(typed) » au lieu de 10.0.2.2 : effacement et nouvelle saisie ($attempt)"
    for i in $(seq 1 12); do erase; done
    type_address
    sleep 1
done
echo "adresse composee : $(typed)"
adb exec-out screencap -p > "$OUT/android-2-adresse.png"

# CONNECTER : bouton de 280 x 78 centre en x, a y = 236 + 4*90 + 14 + 39 = 649
press "$(ux 0)" "$(uy 649)"
sleep 12                         # connexion + premieres images de jeu
adb exec-out screencap -p > "$OUT/android-3-jeu.png"

# Appui long sur TIRER : centre a (156, 112) unites du coin bas droit
# (voir Buttons() dans client/touch.c). swipe sur place pendant 3000 ms =
# appui long, en arriere-plan pour capturer pendant le tir.
FX=$(phys_x $(( W - 156 * H / 720 ))); FY=$(phys_y $(( H - 112 * H / 720 )))
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
# Les captures doivent DIFFERER : si les quatre sont identiques, aucun
# appui n'a atteint l'ecran (vu lors de la mise au point). Informatif.
echo "----- empreintes des captures (identiques = ecran fige) -----"
md5sum "$OUT"/android-*.png

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
