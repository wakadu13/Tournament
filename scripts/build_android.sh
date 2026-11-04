#!/bin/bash
# Compile l'APK Android du CLIENT EMA Tournament, sans Gradle.
#
# Le serveur et le bot ne sont pas concernes : ils tournent sur une machine
# Linux (voir la section "Serveur dedie" du README), pas sur le telephone.
#
# Prerequis (deja presents sur les machines GitHub Actions) :
#   ANDROID_HOME : SDK Android (build-tools + platforms)
#   ANDROID_NDK  : NDK Android
#   RAYLIB_SRC   : sources de raylib 5.5
# Resultat : $OUT/EMATournament-android.apk
#            (arm64-v8a pour les telephones, x86_64 pour les emulateurs)
#
# Etapes :
#   1. pour chaque architecture (ABI) : compiler raylib + la colle
#      native_app_glue du NDK, puis le client, en une seule bibliotheque
#      partagee libmain.so ;
#   2. empaqueter manifeste, ressources et .so dans un APK (aapt) ;
#   3. aligner (zipalign) puis signer (apksigner).
#
# set -e : arret a la premiere erreur ; -u : erreur si variable non definie ;
# -o pipefail : une commande qui echoue dans un pipe fait echouer le pipe.
set -euo pipefail

API=24                       # Android 7.0 minimum
OUT=${OUT:-build-android}
# ${VAR:?message} : arrete le script avec ce message si la variable est vide
NDK=${ANDROID_NDK:?ANDROID_NDK manquant}
SDK=${ANDROID_HOME:?ANDROID_HOME manquant}
RAYLIB=${RAYLIB_SRC:?RAYLIB_SRC manquant}

# Version la plus recente des build-tools et de la plateforme installees
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
PLATFORM_JAR=$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
echo "build-tools : $BT"
echo "plateforme  : $PLATFORM_JAR"

rm -rf "$OUT"
mkdir -p "$OUT/apk"

# native_app_glue : la couche C du NDK qui relie l'activite Java
# NativeActivity a la boucle principale C de raylib (android_main)
GLUE="$NDK/sources/android/native_app_glue"
RAYLIB_MODULES="rcore rshapes rtextures rtext rmodels raudio utils"

# Les sources du client : son propre code + les regles partagees.
# Surtout PAS src/server/main.c ni src/bot/main.c, qui ont leur propre main().
GAME_SRC="src/client/main.c src/client/models.c src/client/weapons.c src/client/touch.c src/client/ui.c src/client/menu.c src/client/options.c src/common/game.c"

for ABI in arm64-v8a x86_64; do
    case $ABI in
        arm64-v8a) TRIPLE=aarch64-linux-android ;;
        x86_64)    TRIPLE=x86_64-linux-android ;;
    esac
    # Le compilateur du NDK porte la cible Android dans son nom.
    # raylib est compilee ici directement : son Makefile 5.5 ajoute un
    # "-target aarch64" qui ecrase cette cible avec les NDK recents et fait
    # inclure les en-tetes Linux de la machine hote.
    CC="$TOOLCHAIN/bin/${TRIPLE}${API}-clang"
    OBJ="$OUT/obj-$ABI"
    mkdir -p "$OBJ" "$OUT/apk/lib/$ABI"
    # Optimisation, code relogeable (obligatoire dans un .so), sections
    # separees (le lieur peut jeter le code inutile), tables de deroulement
    # (piles d'appels lisibles en cas de plantage), protection de pile
    COMMON="-O2 -fPIC -ffunction-sections -funwind-tables -fstack-protector-strong"

    echo "=== raylib pour $ABI"
    for m in $RAYLIB_MODULES; do
        $CC -std=gnu99 $COMMON -DPLATFORM_ANDROID -DGRAPHICS_API_OPENGL_ES2 \
            -I"$RAYLIB/src" -I"$GLUE" -c "$RAYLIB/src/$m.c" -o "$OBJ/rl_$m.o"
    done
    $CC -std=gnu99 $COMMON -I"$GLUE" -c "$GLUE/android_native_app_glue.c" \
        -o "$OBJ/android_native_app_glue.o"

    # Le client. -DPLATFORM_ANDROID active chez NOUS l'ecran de connexion au
    # doigt et les commandes tactiles (voir MOBILE_UI dans client/main.c et
    # TOUCH_ENABLED dans client/touch.c).
    # Bibliotheques systeme : log (logcat), android, EGL + GLESv2 (affichage),
    # OpenSLES (son), m (maths).
    # -u ANativeActivity_onCreate : garde ce point d'entree, sinon le lieur le
    # retire (rien ne le reference) et l'application ne demarre pas.
    # --no-undefined : erreur au lien si un symbole manque, plutot qu'un
    # plantage au lancement.
    echo "=== client pour $ABI"
    $CC -std=c11 $COMMON -shared -Wall -Wextra -DPLATFORM_ANDROID \
        -Isrc -I"$RAYLIB/src" $GAME_SRC "$OBJ"/*.o \
        -o "$OUT/apk/lib/$ABI/libmain.so" \
        -llog -landroid -lEGL -lGLESv2 -lOpenSLES -lm \
        -u ANativeActivity_onCreate -Wl,--no-undefined
done

echo "=== APK"
# aapt compile manifeste et ressources en un APK sans code natif
"$BT/aapt" package -f -M android/AndroidManifest.xml -S android/res \
    -I "$PLATFORM_JAR" -F "$OUT/unaligned.apk"
# Puis on y ajoute les bibliotheques natives. On se place dans apk/ pour que
# les chemins DANS l'archive soient bien lib/<abi>/libmain.so
(cd "$OUT/apk" && "$BT/aapt" add ../unaligned.apk \
    lib/arm64-v8a/libmain.so lib/x86_64/libmain.so)
# Alignement sur 4 octets (exige par Android, et avant la signature)
"$BT/zipalign" -f -p 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
# Cle de debug commitee dans le depot : une mise a jour s'installe par-dessus
# l'ancienne version au lieu d'etre refusee pour cause de signature differente
"$BT/apksigner" sign --ks android/debug.keystore \
    --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/EMATournament-android.apk" "$OUT/aligned.apk"
"$BT/apksigner" verify "$OUT/EMATournament-android.apk"
ls -la "$OUT/EMATournament-android.apk"
