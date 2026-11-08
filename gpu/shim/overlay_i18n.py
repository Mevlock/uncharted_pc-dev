# The in-game menu's translations (the overlay, Insert or L3+R3): the one place they are edited.
# After a change run `python tools/make_overlay_i18n.py` (writes bbport_overlay_i18n.inc, which the build uses) and
# `python tools/make_overlay_i18n.py --check` (every text marked T(), TI(), N_() or tr() in bbport_overlay.cpp and
# bbport_saves.cpp has all six languages with the same %s / {} placeholders). New Japanese characters also need the
# font subset rebuilt: tools/make_overlay_font_ja.py.
#
# Wording follows the launcher's (launcher/uncharted_i18n*.py): tú / você / vous / du / tu, Japanese polite UI style.
# Checkpoint names and chapter titles are the games' own and stay English.


def L(es, pt, fr, de, it, ja):
    return {'es': es, 'pt': pt, 'fr': fr, 'de': de, 'it': it, 'ja': ja}


TRANSLATIONS = {
    # ---- Settings page -------------------------------------------------------------------------------------------
    'Uncharted — settings  (Insert / L3+R3)': L(
        'Uncharted — ajustes  (Insert / L3+R3)', 'Uncharted — configurações  (Insert / L3+R3)',
        'Uncharted — réglages  (Insert / L3+R3)', 'Uncharted — Einstellungen  (Insert / L3+R3)',
        'Uncharted — impostazioni  (Insert / L3+R3)', 'Uncharted — 設定  (Insert / L3+R3)'),
    '%.0f FPS  (%.1f ms)   1%% low %.0f': L(
        '%.0f FPS  (%.1f ms)   1%% mín. %.0f', '%.0f FPS  (%.1f ms)   1%% mín. %.0f',
        '%.0f FPS  (%.1f ms)   1%% min. %.0f', '%.0f FPS  (%.1f ms)   1%% Tief %.0f',
        '%.0f FPS  (%.1f ms)   1%% min. %.0f', '%.0f FPS  (%.1f ms)   下位1%% %.0f'),
    "Changes here apply at once, for this session. The launcher's settings apply each time the game starts: set the "
    "same there to keep a change.": L(
        'Los cambios aquí se aplican al instante, solo en esta sesión. Los ajustes del launcher se aplican cada vez que '
        'se inicia el juego: configura lo mismo allí para conservar un cambio.',
        'As mudanças aqui valem na hora, só nesta sessão. As configurações do launcher valem toda vez que o jogo '
        'inicia: defina o mesmo lá para manter uma mudança.',
        'Les changements ici s’appliquent tout de suite, pour cette session. Les réglages du launcher s’appliquent à '
        'chaque démarrage du jeu : faites le même réglage là-bas pour garder un changement.',
        'Änderungen hier gelten sofort, für diese Sitzung. Die Einstellungen des Launchers gelten bei jedem '
        'Spielstart: Stell dort dasselbe ein, um eine Änderung zu behalten.',
        "Le modifiche qui valgono subito, per questa sessione. Le impostazioni del launcher valgono a ogni avvio del "
        "gioco: imposta lo stesso lì per mantenere una modifica.",
        'ここでの変更はすぐに反映され、このセッションのみ有効です。ランチャーの設定はゲーム起動時に毎回適用されます。'
        '変更を残すには、ランチャーでも同じ設定にしてください。'),
    'Settings': L('Ajustes', 'Configurações', 'Réglages', 'Einstellungen', 'Impostazioni', '設定'),
    'Picture': L('Imagen', 'Imagem', 'Image', 'Bild', 'Immagine', '画質'),
    'Rendering %s': L('Renderizado: %s', 'Renderização: %s', 'Rendu : %s', 'Rendern: %s', 'Rendering: %s',
                      'レンダリング: %s'),
    "(the game's own)": L('(la del juego)', '(a do jogo)', '(celle du jeu)', '(die des Spiels)',
                          '(quella del gioco)', '(ゲーム本来)'),
    '(native)': L('(nativa)', '(nativa)', '(native)', '(nativ)', '(nativa)', '(ネイティブ)'),
    '(Quality)': L('(Calidad)', '(Qualidade)', '(Qualité)', '(Qualität)', '(Qualità)', '(クオリティ)'),
    '(Balanced)': L('(Equilibrado)', '(Equilibrado)', '(Équilibré)', '(Ausgewogen)', '(Bilanciata)', '(バランス)'),
    '(Performance)': L('(Rendimiento)', '(Desempenho)', '(Performance)', '(Leistung)', '(Prestazioni)',
                       '(パフォーマンス)'),
    'Set in the launcher (Display: Render resolution); it applies when the game starts. Quality, Balanced and '
    'Performance render smaller and let the anti-aliasing (DLSS or FSR) rebuild the full size: Uncharted 3 only.': L(
        'Se ajusta en el launcher (Pantalla: Resolución de renderizado) y se aplica al iniciar el juego. Calidad, '
        'Equilibrado y Rendimiento renderizan más pequeño y dejan que el suavizado de bordes (DLSS o FSR) reconstruya '
        'el tamaño completo: solo Uncharted 3.',
        'Definido no launcher (Tela: Resolução de renderização); vale quando o jogo inicia. Qualidade, Equilibrado e '
        'Desempenho renderizam menor e deixam o antisserrilhamento (DLSS ou FSR) reconstruir o tamanho completo: só '
        'Uncharted 3.',
        'Se règle dans le launcher (Affichage : Résolution de rendu) et s’applique au démarrage du jeu. Qualité, '
        'Équilibré et Performance rendent plus petit et laissent l’anticrénelage (DLSS ou FSR) reconstruire la taille '
        'réelle : Uncharted 3 uniquement.',
        'Wird im Launcher eingestellt (Anzeige: Renderauflösung) und gilt ab dem Spielstart. Qualität, Ausgewogen und '
        'Leistung rendern kleiner und lassen die Kantenglättung (DLSS oder FSR) die volle Größe rekonstruieren: nur '
        'Uncharted 3.',
        "Si imposta nel launcher (Schermo: Risoluzione di rendering) e vale all'avvio del gioco. Qualità, Bilanciata e "
        "Prestazioni renderizzano più in piccolo e lasciano che l'antialiasing (DLSS o FSR) ricostruisca la "
        "dimensione piena: solo Uncharted 3.",
        'ランチャーで設定します(画面: レンダリング解像度)。ゲーム起動時に適用されます。クオリティ、バランス、'
        'パフォーマンスは小さく描画し、アンチエイリアス(DLSS または FSR)でフルサイズに再構築します。Uncharted 3 のみ。'),
    "Off (the game's own)": L('Desactivado (el del juego)', 'Desligado (o do jogo)', 'Désactivé (celui du jeu)',
                              'Aus (das des Spiels)', 'Disattivo (quello del gioco)', 'オフ(ゲーム本来)'),
    'Anti-aliasing': L('Suavizado de bordes', 'Antisserrilhamento', 'Anticrénelage', 'Kantenglättung',
                       'Antialiasing', 'アンチエイリアス'),
    '— not supported by this graphics card': L(
        '— no compatible con esta tarjeta gráfica', '— não suportado por esta placa de vídeo',
        '— non pris en charge par cette carte graphique', '— von dieser Grafikkarte nicht unterstützt',
        '— non supportato da questa scheda video', '— このグラフィックカードでは非対応'),
    'FSR 4 unavailable: %s': L('FSR 4 no disponible: %s', 'FSR 4 indisponível: %s', 'FSR 4 indisponible : %s',
                               'FSR 4 nicht verfügbar: %s', 'FSR 4 non disponibile: %s', 'FSR 4 は利用できません: %s'),
    'The mode selected above is active. FSR 4 can be selected again.': L(
        'El modo elegido arriba está activo. FSR 4 se puede volver a elegir.',
        'O modo escolhido acima está ativo. O FSR 4 pode ser escolhido de novo.',
        'Le mode choisi ci-dessus est actif. FSR 4 peut être choisi à nouveau.',
        'Der oben gewählte Modus ist aktiv. FSR 4 kann wieder gewählt werden.',
        'La modalità scelta sopra è attiva. FSR 4 può essere scelto di nuovo.',
        '上で選んだモードが有効です。FSR 4 は再び選択できます。'),
    # Problems set elsewhere (bbport_settings.cpp, vk_frame_generation.cpp) and shown with T(problem).
    'GPU does not support the selected FSR 4 shaders; using FSR 3.1': L(
        'La GPU no admite los shaders de FSR 4 elegidos; se usa FSR 3.1',
        'A GPU não suporta os shaders do FSR 4 escolhidos; usando FSR 3.1',
        'Le GPU ne prend pas en charge les shaders FSR 4 choisis ; FSR 3.1 est utilisé',
        'Die GPU unterstützt die gewählten FSR-4-Shader nicht; FSR 3.1 wird verwendet',
        'La GPU non supporta gli shader FSR 4 scelti; si usa FSR 3.1',
        'GPU が選択した FSR 4 シェーダーに対応していないため、FSR 3.1 を使用します'),
    'DLSS Frame Generation needs a GeForce RTX 40 or newer': L(
        'DLSS Frame Generation necesita una GeForce RTX 40 o más reciente',
        'O DLSS Frame Generation precisa de uma GeForce RTX 40 ou mais recente',
        'DLSS Frame Generation nécessite une GeForce RTX 40 ou plus récente',
        'DLSS Frame Generation braucht eine GeForce RTX 40 oder neuer',
        'DLSS Frame Generation richiede una GeForce RTX 40 o più recente',
        'DLSS フレーム生成には GeForce RTX 40 以降が必要です'),
    'Smooths edges and keeps the picture stable from frame to frame; with an upscaled render resolution it also '
    'rebuilds the full size. DLSS needs a GeForce RTX card.': L(
        'Suaviza los bordes y mantiene la imagen estable de un cuadro a otro; con una resolución de renderizado '
        'reescalada también reconstruye el tamaño completo. DLSS necesita una tarjeta GeForce RTX.',
        'Suaviza as bordas e mantém a imagem estável de um quadro para o outro; com uma resolução de renderização '
        'reduzida também reconstrói o tamanho completo. O DLSS precisa de uma placa GeForce RTX.',
        'Lisse les bords et garde l’image stable d’une image à l’autre ; avec une résolution de rendu mise à '
        'l’échelle, il reconstruit aussi la taille réelle. DLSS nécessite une carte GeForce RTX.',
        'Glättet Kanten und hält das Bild von Frame zu Frame stabil; mit hochskalierter Renderauflösung rekonstruiert '
        'sie auch die volle Größe. DLSS braucht eine GeForce-RTX-Karte.',
        "Smussa i bordi e mantiene l'immagine stabile da un fotogramma all'altro; con una risoluzione di rendering "
        "ridotta ricostruisce anche la dimensione piena. DLSS richiede una scheda GeForce RTX.",
        'エッジを滑らかにし、フレーム間で画像を安定させます。アップスケールするレンダリング解像度では'
        'フルサイズへの再構築も行います。DLSS には GeForce RTX カードが必要です。'),
    'Sharpness': L('Intensidad de nitidez', 'Intensidade da nitidez', 'Intensité de la netteté', 'Schärfestärke',
                   'Intensità della nitidezza', 'シャープの強さ'),
    "Sharpens the anti-aliased picture (AMD RCAS, also with DLSS). 0 is off; the launcher's Low, Medium and High are "
    "0.2, 0.4 and 0.7. Ctrl+click to type a value.": L(
        'Aumenta la nitidez de la imagen suavizada (AMD RCAS, también con DLSS). 0 es desactivado; Bajo, Medio y Alto '
        'del launcher son 0.2, 0.4 y 0.7. Ctrl+clic para escribir un valor.',
        'Deixa mais nítida a imagem suavizada (AMD RCAS, também com DLSS). 0 é desligado; Baixo, Médio e Alto do '
        'launcher são 0.2, 0.4 e 0.7. Ctrl+clique para digitar um valor.',
        'Accentue la netteté de l’image lissée (AMD RCAS, aussi avec DLSS). 0 = désactivé ; Faible, Moyen et Élevé du '
        'launcher valent 0.2, 0.4 et 0.7. Ctrl+clic pour saisir une valeur.',
        'Schärft das geglättete Bild (AMD RCAS, auch mit DLSS). 0 ist aus; Niedrig, Mittel und Hoch im Launcher sind '
        '0.2, 0.4 und 0.7. Strg+Klick, um einen Wert einzugeben.',
        "Rende più nitida l'immagine con antialiasing (AMD RCAS, anche con DLSS). 0 è disattivo; Bassa, Media e Alta "
        "del launcher sono 0.2, 0.4 e 0.7. Ctrl+clic per digitare un valore.",
        'アンチエイリアス後の画像をシャープにします(AMD RCAS、DLSS でも有効)。0 でオフ。ランチャーの低・中・高は'
        ' 0.2、0.4、0.7 です。Ctrl+クリックで値を入力できます。'),
    'Frame generation': L('Generación de cuadros', 'Geração de quadros', 'Génération d’images', 'Frame-Generierung',
                          'Generazione fotogrammi', 'フレーム生成'),
    'Off': L('Desactivado', 'Desligado', 'Désactivé', 'Aus', 'Disattivo', 'オフ'),
    'FSR 3 (any graphics card)': L('FSR 3 (cualquier tarjeta gráfica)', 'FSR 3 (qualquer placa de vídeo)',
                                   'FSR 3 (toute carte graphique)', 'FSR 3 (jede Grafikkarte)',
                                   'FSR 3 (qualsiasi scheda video)', 'FSR 3 (すべてのグラフィックカード)'),
    'Multiplier': L('Multiplicador', 'Multiplicador', 'Multiplicateur', 'Multiplikator', 'Moltiplicatore', '倍率'),
    "Generated frames between the game's own: 60 FPS on screen becomes 120. Adds a little input latency; menus and "
    "cutscene videos are not generated. The launcher's Frame rate limit counts generated frames.": L(
        'Cuadros generados entre los del juego: 60 FPS en pantalla pasan a ser 120. Añade un poco de latencia de '
        'entrada; los menús y los vídeos de las cinemáticas no se generan. El Límite de cuadros por segundo del '
        'launcher cuenta los cuadros generados.',
        'Quadros gerados entre os do jogo: 60 FPS na tela viram 120. Adiciona um pouco de latência de entrada; menus '
        'e vídeos de cenas não são gerados. O Limite de quadros por segundo do launcher conta os quadros gerados.',
        'Des images générées entre celles du jeu : 60 FPS à l’écran deviennent 120. Ajoute un peu de latence ; les '
        'menus et les vidéos des cinématiques ne sont pas générés. La Limite d’images par seconde du launcher compte '
        'les images générées.',
        'Generierte Frames zwischen denen des Spiels: Aus 60 FPS auf dem Bildschirm werden 120. Erhöht die '
        'Eingabelatenz etwas; Menüs und Zwischensequenz-Videos werden nicht generiert. Die Bildratenbegrenzung des '
        'Launchers zählt generierte Frames mit.',
        "Fotogrammi generati tra quelli del gioco: 60 FPS a schermo diventano 120. Aggiunge un po' di latenza di "
        "input; menu e video dei filmati non vengono generati. Il Limite fotogrammi del launcher conta i fotogrammi "
        "generati.",
        'ゲーム本来のフレームの間にフレームを生成します。画面上の 60 FPS が 120 になります。入力遅延が少し増えます。'
        'メニューとカットシーンの動画は生成されません。ランチャーのフレームレート上限は生成フレームも数えます。'),
    'Mouse': L('Ratón', 'Mouse', 'Souris', 'Maus', 'Mouse', 'マウス'),
    'Mouse sensitivity': L('Sensibilidad del ratón', 'Sensibilidade do mouse', 'Sensibilité de la souris',
                           'Mausempfindlichkeit', 'Sensibilità del mouse', 'マウス感度'),
    'Aiming sensitivity': L('Sensibilidad al apuntar', 'Sensibilidade ao mirar', 'Sensibilité en visée',
                            'Empfindlichkeit beim Zielen', 'Sensibilità in mira', '照準時の感度'),
    'Vertical sensitivity': L('Sensibilidad vertical', 'Sensibilidade vertical', 'Sensibilité verticale',
                              'Vertikale Empfindlichkeit', 'Sensibilità verticale', '縦方向の感度'),
    'Aiming: times the mouse sensitivity while aiming (1.00 the same as looking around). Vertical: times it for '
    'up and down. Ctrl+click to type a value.': L(
        'Al apuntar: multiplica la sensibilidad del ratón mientras apuntas (1.00 igual que al mirar alrededor). '
        'Vertical: la multiplica para arriba y abajo. Ctrl+clic para escribir un valor.',
        'Ao mirar: multiplica a sensibilidade do mouse enquanto você mira (1.00 igual a olhar ao redor). Vertical: '
        'multiplica para cima e para baixo. Ctrl+clique para digitar um valor.',
        'En visée : multiplie la sensibilité de la souris pendant la visée (1.00 = comme pour regarder autour). '
        'Verticale : la multiplie pour le haut et le bas. Ctrl+clic pour saisir une valeur.',
        'Beim Zielen: Faktor auf die Mausempfindlichkeit beim Zielen (1.00 wie beim Umsehen). Vertikal: Faktor für '
        'oben und unten. Strg+Klick, um einen Wert einzugeben.',
        'In mira: moltiplica la sensibilità del mouse mentre miri (1.00 come quando ti guardi intorno). Verticale: '
        'la moltiplica per su e giù. Ctrl+clic per digitare un valore.',
        '照準時：照準中のマウス感度の倍率です(1.00 で見回すときと同じ)。縦方向：上下の動きの倍率です。'
        'Ctrl+クリックで値を入力できます。'),
    'Other': L('Otros', 'Outros', 'Autres', 'Sonstiges', 'Altro', 'その他'),
    'FPS counter in the corner': L('Contador de FPS en la esquina', 'Contador de FPS no canto',
                                   'Compteur de FPS dans le coin', 'FPS-Zähler in der Ecke',
                                   "Contatore FPS nell'angolo", '画面隅に FPS カウンター'),
    'Frame time graph': L('Gráfico de tiempos de cuadro', 'Gráfico de tempo de quadro', 'Graphique des temps d’image',
                          'Frametime-Diagramm', 'Grafico dei tempi dei fotogrammi', 'フレームタイムのグラフ'),
    "Top line, the game's own frames: FPS and frame time, 1%L the average of the slowest 1% (stutter), MAX the "
    "slowest frame (last 5 seconds). Below: the anti-aliasing, the render size (Q, B, P: Quality, Balanced, "
    "Performance) and with frame generation FG, the frames on screen. The graph: the game's frame times, a spike is "
    "a hitch.": L(
        'Línea superior, los cuadros del propio juego: FPS y tiempo de cuadro, 1%L la media del 1% más lento '
        '(tirones), MAX el cuadro más lento (últimos 5 segundos). Debajo: el suavizado de bordes, el tamaño de '
        'renderizado (Q, B, P: Calidad, Equilibrado, Rendimiento) y, con generación de cuadros, FG y los cuadros en '
        'pantalla. El gráfico: los tiempos de cuadro del juego; un pico es un tirón.',
        'Linha de cima, os quadros do próprio jogo: FPS e tempo de quadro, 1%L a média do 1% mais lento (engasgos), '
        'MAX o quadro mais lento (últimos 5 segundos). Abaixo: o antisserrilhamento, o tamanho de renderização (Q, B, '
        'P: Qualidade, Equilibrado, Desempenho) e, com geração de quadros, FG e os quadros na tela. O gráfico: os '
        'tempos de quadro do jogo; um pico é um engasgo.',
        'Ligne du haut, les images du jeu lui-même : FPS et temps d’image, 1%L la moyenne du 1% le plus lent '
        '(saccades), MAX l’image la plus lente (5 dernières secondes). En dessous : l’anticrénelage, la taille de rendu '
        '(Q, B, P : Qualité, Équilibré, Performance) et, avec la génération d’images, FG et les images à l’écran. Le '
        'graphique : les temps d’image du jeu ; un pic est une saccade.',
        'Obere Zeile, die Frames des Spiels selbst: FPS und Frametime, 1%L der Durchschnitt der langsamsten 1% '
        '(Ruckler), MAX der langsamste Frame (letzte 5 Sekunden). Darunter: die Kantenglättung, die Rendergröße (Q, B, '
        'P: Qualität, Ausgewogen, Leistung) und mit Frame-Generierung FG und die Frames auf dem Bildschirm. Das '
        'Diagramm: die Frametimes des Spiels, eine Spitze ist ein Ruckler.',
        "Riga in alto, i fotogrammi del gioco stesso: FPS e tempo per fotogramma, 1%L la media dell'1% più lento "
        "(scatti), MAX il fotogramma più lento (ultimi 5 secondi). Sotto: l'antialiasing, la dimensione di rendering "
        "(Q, B, P: Qualità, Bilanciata, Prestazioni) e, con la generazione fotogrammi, FG e i fotogrammi a schermo. "
        "Il grafico: i tempi dei fotogrammi del gioco; un picco è uno scatto.",
        '上の行はゲーム本来のフレーム: FPS とフレームタイム、1%L は最も遅い 1% の平均(カクつき)、MAX は最も遅い'
        'フレーム(直近 5 秒)。その下: アンチエイリアス、レンダリングサイズ(Q、B、P: クオリティ、バランス、'
        'パフォーマンス)、フレーム生成時は FG と画面上のフレーム数。グラフ: ゲームのフレームタイムで、'
        '突き出た山はカクつきです。'),
    'Developer options': L('Opciones de desarrollador', 'Opções de desenvolvedor', 'Options de développement',
                           'Entwickleroptionen', 'Opzioni sviluppatore', '開発者向けオプション'),
    'Sub-pixel jitter': L('Jitter de subpíxel', 'Jitter de subpixel', 'Jitter sous-pixel', 'Subpixel-Jitter',
                          'Jitter sub-pixel', 'サブピクセルジッター'),
    'Each frame the scene shifts by a fraction of a pixel and the anti-aliasing gathers detail from several frames. '
    'Off: smoothing from the history only.': L(
        'En cada cuadro la escena se desplaza una fracción de píxel y el suavizado de bordes reúne detalle de varios '
        'cuadros. Desactivado: solo suavizado a partir del historial.',
        'A cada quadro a cena se desloca uma fração de pixel e o antisserrilhamento reúne detalhes de vários quadros. '
        'Desligado: suavização só a partir do histórico.',
        'À chaque image, la scène se décale d’une fraction de pixel et l’anticrénelage rassemble les détails de '
        'plusieurs images. Désactivé : lissage à partir de l’historique seulement.',
        'In jedem Frame verschiebt sich die Szene um einen Bruchteil eines Pixels, und die Kantenglättung sammelt '
        'Details aus mehreren Frames. Aus: Glättung nur aus dem Verlauf.',
        "A ogni fotogramma la scena si sposta di una frazione di pixel e l'antialiasing raccoglie dettagli da più "
        "fotogrammi. Disattivo: smussatura solo dalla cronologia.",
        'フレームごとにシーンを 1 ピクセル未満ずらし、アンチエイリアスが複数のフレームから細部を集めます。'
        'オフ: 履歴だけで滑らかにします。'),
    'Sharpening pass': L('Pase de nitidez', 'Passo de nitidez', 'Passe de netteté', 'Schärfungsdurchgang',
                         'Passaggio di nitidezza', 'シャープネス処理'),
    'FSR 4: auto exposure': L('FSR 4: exposición automática', 'FSR 4: exposição automática',
                              'FSR 4 : exposition automatique', 'FSR 4: automatische Belichtung',
                              'FSR 4: esposizione automatica', 'FSR 4: 自動露出'),
    'FSR 4: inverted jitter sign': L('FSR 4: signo del jitter invertido', 'FSR 4: sinal do jitter invertido',
                                     'FSR 4 : signe du jitter inversé', 'FSR 4: Jitter-Vorzeichen umgekehrt',
                                     'FSR 4: segno del jitter invertito', 'FSR 4: ジッターの符号を反転'),
    'Reactive mask': L('Máscara reactiva', 'Máscara reativa', 'Masque réactif', 'Reaktive Maske', 'Maschera reattiva',
                       'リアクティブマスク'),
    'Marks transparent effects (particles, haze) so the upscaler relies less on past frames: fewer trails behind '
    'effects, more shimmer under them.': L(
        'Marca los efectos transparentes (partículas, neblina) para que el reescalador dependa menos de los cuadros '
        'anteriores: menos estelas tras los efectos, más parpadeo bajo ellos.',
        'Marca efeitos transparentes (partículas, névoa) para o upscaler depender menos dos quadros anteriores: menos '
        'rastros atrás dos efeitos, mais cintilação sob eles.',
        'Marque les effets transparents (particules, brume) pour que l’upscaler s’appuie moins sur les images '
        'passées : moins de traînées derrière les effets, plus de scintillement dessous.',
        'Markiert transparente Effekte (Partikel, Dunst), damit sich der Upscaler weniger auf frühere Frames stützt: '
        'weniger Schlieren hinter Effekten, mehr Flimmern darunter.',
        "Segna gli effetti trasparenti (particelle, foschia) così l'upscaler si affida meno ai fotogrammi passati: "
        "meno scie dietro gli effetti, più sfarfallio sotto.",
        '透明なエフェクト(パーティクル、もや)をマークし、アップスケーラーが過去のフレームに頼りすぎないように'
        'します。エフェクトの残像は減り、その下のちらつきは増えます。'),
    'Mask scale': L('Escala de la máscara', 'Escala da máscara', 'Échelle du masque', 'Maskenskalierung',
                    'Scala della maschera', 'マスクの倍率'),
    'Mask threshold': L('Umbral de la máscara', 'Limiar da máscara', 'Seuil du masque', 'Maskenschwelle',
                        'Soglia della maschera', 'マスクのしきい値'),
    'Mask maximum': L('Máximo de la máscara', 'Máximo da máscara', 'Maximum du masque', 'Maskenmaximum',
                      'Massimo della maschera', 'マスクの最大値'),
    'Show the mask': L('Mostrar la máscara', 'Mostrar a máscara', 'Afficher le masque', 'Maske anzeigen',
                       'Mostra la maschera', 'マスクを表示'),
    'Character motion vectors': L('Vectores de movimiento de personajes', 'Vetores de movimento dos personagens',
                                  'Vecteurs de mouvement des personnages', 'Bewegungsvektoren für Figuren',
                                  'Vettori di movimento dei personaggi', 'キャラクターのモーションベクター'),
    'Exact vectors for animated objects (applies after restarting the game).': L(
        'Vectores exactos para los objetos animados (se aplica tras reiniciar el juego).',
        'Vetores exatos para objetos animados (vale depois de reiniciar o jogo).',
        'Vecteurs exacts pour les objets animés (s’applique après le redémarrage du jeu).',
        'Exakte Vektoren für animierte Objekte (gilt nach einem Neustart des Spiels).',
        'Vettori esatti per gli oggetti animati (vale dopo il riavvio del gioco).',
        'アニメーションするオブジェクトの正確なベクター(ゲームの再起動後に適用)。'),
    'Show the motion vectors': L('Mostrar los vectores de movimiento', 'Mostrar os vetores de movimento',
                                 'Afficher les vecteurs de mouvement', 'Bewegungsvektoren anzeigen',
                                 'Mostra i vettori di movimento', 'モーションベクターを表示'),
    'Red/green: horizontal/vertical motion (8 pixels = full brightness); blue: an exact object vector.': L(
        'Rojo/verde: movimiento horizontal/vertical (8 píxeles = brillo máximo); azul: un vector exacto de objeto.',
        'Vermelho/verde: movimento horizontal/vertical (8 pixels = brilho máximo); azul: um vetor exato de objeto.',
        'Rouge/vert : mouvement horizontal/vertical (8 pixels = luminosité maximale) ; bleu : un vecteur d’objet '
        'exact.',
        'Rot/Grün: horizontale/vertikale Bewegung (8 Pixel = volle Helligkeit); Blau: ein exakter Objektvektor.',
        'Rosso/verde: movimento orizzontale/verticale (8 pixel = luminosità piena); blu: un vettore esatto di un '
        'oggetto.',
        '赤/緑: 水平/垂直方向の動き(8 ピクセルで最大の明るさ)。青: オブジェクトの正確なベクター。'),
    'Changes apply after restarting the game': L(
        'Los cambios se aplican tras reiniciar el juego', 'As mudanças valem depois de reiniciar o jogo',
        'Les changements s’appliquent après le redémarrage du jeu', 'Änderungen gelten nach einem Neustart des Spiels',
        'Le modifiche valgono dopo il riavvio del gioco', '変更はゲームの再起動後に適用されます'),
    'Apply and restart the game': L('Aplicar y reiniciar el juego', 'Aplicar e reiniciar o jogo',
                                    'Appliquer et redémarrer le jeu', 'Übernehmen und Spiel neu starten',
                                    'Applica e riavvia il gioco', '適用してゲームを再起動'),
    'Checkpoints': L('Puntos de control', 'Checkpoints', 'Points de contrôle', 'Checkpoints', 'Checkpoint',
                     'チェックポイント'),
    'Save slots': L('Ranuras de guardado', 'Slots de salvamento', 'Emplacements de sauvegarde', 'Speicherplätze',
                    'Slot di salvataggio', 'セーブスロット'),
    'Close': L('Cerrar', 'Fechar', 'Fermer', 'Schließen', 'Chiudi', '閉じる'),
    'Cancel': L('Cancelar', 'Cancelar', 'Annuler', 'Abbrechen', 'Annulla', 'キャンセル'),
    'Yes': L('Sí', 'Sim', 'Oui', 'Ja', 'Sì', 'はい'),

    # ---- Checkpoints page ----------------------------------------------------------------------------------------
    'Done.': L('Hecho.', 'Pronto.', 'Terminé.', 'Erledigt.', 'Fatto.', '完了しました。'),
    'This checkpoint is not in this release of the game.': L(
        'Este punto de control no existe en esta versión del juego.',
        'Este checkpoint não existe nesta versão do jogo.',
        'Ce point de contrôle n’existe pas dans cette version du jeu.',
        'Diesen Checkpoint gibt es in dieser Version des Spiels nicht.',
        'Questo checkpoint non esiste in questa versione del gioco.',
        'このチェックポイントはこのバージョンのゲームにはありません。'),
    'Checkpoint jumps do not work with this release of the game.': L(
        'Los saltos a puntos de control no funcionan con esta versión del juego.',
        'Os saltos para checkpoints não funcionam com esta versão do jogo.',
        'Les sauts vers un point de contrôle ne fonctionnent pas avec cette version du jeu.',
        'Checkpoint-Sprünge funktionieren mit dieser Version des Spiels nicht.',
        'I salti ai checkpoint non funzionano con questa versione del gioco.',
        'このバージョンのゲームではチェックポイントへのジャンプは使えません。'),
    'Checkpoint jumps are not available for this release of the game.': L(
        'Los saltos a puntos de control no están disponibles para esta versión del juego.',
        'Os saltos para checkpoints não estão disponíveis para esta versão do jogo.',
        'Les sauts vers un point de contrôle ne sont pas disponibles pour cette version du jeu.',
        'Checkpoint-Sprünge sind für diese Version des Spiels nicht verfügbar.',
        'I salti ai checkpoint non sono disponibili per questa versione del gioco.',
        'このバージョンのゲームではチェックポイントへのジャンプは利用できません。'),
    'Now: %s': L('Ahora: %s', 'Agora: %s', 'Actuellement : %s', 'Jetzt: %s', 'Ora: %s', '現在: %s'),
    'Practice mode: the game saves to a practice copy until you restart it. Your own save and story save are not '
    'touched.': L(
        'Modo práctica: el juego guarda en una copia de práctica hasta que lo reinicies. Tu partida y tu partida de la '
        'historia no se tocan.',
        'Modo de treino: o jogo salva em uma cópia de treino até você reiniciá-lo. Seu jogo salvo e o jogo salvo da '
        'história não são alterados.',
        'Mode entraînement : le jeu sauvegarde dans une copie d’entraînement jusqu’à ce que vous le redémarriez. Votre '
        'sauvegarde et votre sauvegarde de l’histoire ne sont pas modifiées.',
        'Übungsmodus: Das Spiel speichert in eine Übungskopie, bis du es neu startest. Dein Spielstand und dein '
        'Story-Spielstand bleiben unberührt.',
        'Modalità pratica: il gioco salva su una copia di pratica finché non lo riavvii. Il tuo salvataggio e il '
        'salvataggio della storia non vengono toccati.',
        '練習モード: ゲームを再起動するまで、ゲームは練習用コピーにセーブします。あなたのセーブデータと'
        'ストーリーセーブは変更されません。'),
    'Keep this progress': L('Conservar este progreso', 'Manter este progresso', 'Garder cette progression',
                            'Diesen Fortschritt behalten', 'Mantieni questi progressi', 'この進行状況を残す'),
    'To carry on from here for real (for example past a place that does not work): the game restarts and Continue '
    'starts at the practice save, which becomes your save and your story save. Your save from before is kept in Save '
    'slots as "Before the last load".': L(
        'Para seguir desde aquí de verdad (por ejemplo, tras un punto que no funciona): el juego se reinicia y '
        'Continuar empieza en la partida de práctica, que pasa a ser tu partida y tu partida de la historia. Tu partida '
        'anterior se conserva en Ranuras de guardado como “Antes de la última carga”.',
        'Para continuar daqui de verdade (por exemplo, depois de um trecho que não funciona): o jogo reinicia e '
        'Continuar começa no jogo salvo de treino, que passa a ser o seu jogo salvo e o jogo salvo da história. Seu '
        'jogo salvo anterior fica em Slots de salvamento como “Antes do último carregamento”.',
        'Pour continuer d’ici pour de bon (par exemple après un passage qui ne fonctionne pas) : le jeu redémarre et '
        'Continuer reprend à la sauvegarde d’entraînement, qui devient votre sauvegarde et votre sauvegarde de '
        'l’histoire. Votre sauvegarde précédente est conservée dans Emplacements de sauvegarde sous « Avant le dernier '
        'chargement ».',
        'Um von hier aus wirklich weiterzuspielen (etwa hinter einer Stelle, die nicht funktioniert): Das Spiel '
        'startet neu und Fortsetzen beginnt beim Übungsspielstand, der dein Spielstand und dein Story-Spielstand wird. '
        'Dein bisheriger Spielstand bleibt unter Speicherplätze als „Vor dem letzten Laden“ erhalten.',
        "Per proseguire davvero da qui (ad esempio oltre un punto che non funziona): il gioco si riavvia e Continua "
        "parte dal salvataggio di pratica, che diventa il tuo salvataggio e il salvataggio della storia. Il "
        "salvataggio precedente resta in Slot di salvataggio come “Prima dell'ultimo caricamento”.",
        'ここから本当に続ける場合(うまく動かない場所を越えた後など): ゲームが再起動し、コンティニューは'
        '練習用セーブから始まります。それがあなたのセーブデータとストーリーセーブになります。以前のセーブデータは'
        'セーブスロットに「前回のロード前」として残ります。'),
    'Make the practice save your save? The game restarts and Continue starts there. Your save from before is kept as '
    '"Before the last load".': L(
        '¿Convertir la partida de práctica en tu partida? El juego se reinicia y Continuar empieza ahí. Tu partida '
        'anterior se conserva como “Antes de la última carga”.',
        'Tornar o jogo salvo de treino o seu jogo salvo? O jogo reinicia e Continuar começa ali. Seu jogo salvo '
        'anterior fica como “Antes do último carregamento”.',
        'Faire de la sauvegarde d’entraînement votre sauvegarde ? Le jeu redémarre et Continuer reprend là. Votre '
        'sauvegarde précédente est conservée sous « Avant le dernier chargement ».',
        'Den Übungsspielstand zu deinem Spielstand machen? Das Spiel startet neu und Fortsetzen beginnt dort. Dein '
        'bisheriger Spielstand bleibt als „Vor dem letzten Laden“ erhalten.',
        "Rendere il salvataggio di pratica il tuo salvataggio? Il gioco si riavvia e Continua parte da lì. Il "
        "salvataggio precedente resta come “Prima dell'ultimo caricamento”.",
        '練習用セーブをあなたのセーブデータにしますか？ゲームが再起動し、コンティニューはそこから始まります。'
        '以前のセーブデータは「前回のロード前」として残ります。'),
    'Yes, keep it': L('Sí, conservarla', 'Sim, manter', 'Oui, la garder', 'Ja, behalten', 'Sì, mantienilo',
                      'はい、残します'),
    'Jumping turns on practice mode: until the game restarts, it saves to a practice copy, not to your own save.': L(
        'Saltar activa el modo práctica: hasta que el juego se reinicie, guarda en una copia de práctica, no en tu '
        'partida.',
        'Saltar ativa o modo de treino: até o jogo reiniciar, ele salva em uma cópia de treino, não no seu jogo salvo.',
        'Un saut active le mode entraînement : jusqu’au redémarrage du jeu, il sauvegarde dans une copie '
        'd’entraînement, pas dans votre sauvegarde.',
        'Ein Sprung schaltet den Übungsmodus ein: Bis das Spiel neu startet, speichert es in eine Übungskopie, nicht '
        'in deinen Spielstand.',
        'Saltare attiva la modalità pratica: finché il gioco non si riavvia, salva su una copia di pratica, non sul '
        'tuo salvataggio.',
        'ジャンプすると練習モードになります。ゲームを再起動するまで、セーブはあなたのセーブデータではなく'
        '練習用コピーに保存されます。'),
    'Chapter': L('Capítulo', 'Capítulo', 'Chapitre', 'Kapitel', 'Capitolo', 'チャプター'),
    '(now)': L('(ahora)', '(agora)', '(actuel)', '(jetzt)', '(ora)', '(現在)'),
    'Go there': L('Ir allí', 'Ir para lá', 'Y aller', 'Dorthin springen', 'Vai lì', 'ジャンプ'),
    'Or double-click a checkpoint in the list.': L(
        'O haz doble clic en un punto de control de la lista.', 'Ou clique duas vezes em um checkpoint da lista.',
        'Ou double-cliquez sur un point de contrôle de la liste.', 'Oder doppelklicke auf einen Checkpoint in der Liste.',
        'Oppure fai doppio clic su un checkpoint nella lista.', 'またはリストのチェックポイントをダブルクリックします。'),

    # ---- Save slots page -----------------------------------------------------------------------------------------
    'Story save': L('Partida de la historia', 'Jogo salvo da história', 'Sauvegarde de l’histoire',
                    'Story-Spielstand', 'Salvataggio della storia', 'ストーリーセーブ'),
    "Starts with the game's next save.": L(
        'Empieza la próxima vez que el juego guarde.', 'Começa no próximo salvamento do jogo.',
        'Commence à la prochaine sauvegarde du jeu.', 'Beginnt mit dem nächsten Speichern des Spiels.',
        'Inizia con il prossimo salvataggio del gioco.', 'ゲームが次にセーブしたときに始まります。'),
    'Your save has moved away from your story (Chapter Select or a new game). The story save stays here until you '
    'load it, or make the current save your story save to keep the change.': L(
        'Tu partida se ha separado de tu historia (Selección de capítulo o una partida nueva). La partida de la '
        'historia se queda aquí hasta que la cargues; o convierte la partida actual en tu partida de la historia para '
        'conservar el cambio.',
        'Seu jogo salvo se afastou da sua história (Seleção de capítulo ou um novo jogo). O jogo salvo da história '
        'fica aqui até você carregá-lo; ou torne o jogo salvo atual o seu jogo salvo da história para manter a '
        'mudança.',
        'Votre sauvegarde s’est éloignée de votre histoire (Sélection du chapitre ou nouvelle partie). La sauvegarde '
        'de l’histoire reste ici jusqu’à ce que vous la chargiez ; ou faites de la sauvegarde actuelle votre '
        'sauvegarde de l’histoire pour garder le changement.',
        'Dein Spielstand hat sich von deiner Story entfernt (Kapitelauswahl oder ein neues Spiel). Der '
        'Story-Spielstand bleibt hier, bis du ihn lädst – oder mach den aktuellen Spielstand zu deinem '
        'Story-Spielstand, um die Änderung zu behalten.',
        'Il tuo salvataggio si è allontanato dalla tua storia (Selezione capitolo o una nuova partita). Il '
        'salvataggio della storia resta qui finché non lo carichi; oppure rendi il salvataggio attuale il salvataggio '
        'della storia per mantenere il cambiamento.',
        'セーブデータがストーリーから外れました(チャプターセレクトまたはニューゲーム)。ストーリーセーブは'
        'ロードするまでここに残ります。変更を残すには、現在のセーブデータをストーリーセーブにしてください。'),
    'Follows your playthrough as you play on.': L(
        'Sigue tu partida mientras juegas.', 'Acompanha sua jornada enquanto você joga.',
        'Suit votre progression à mesure que vous jouez.', 'Folgt deinem Durchgang, während du weiterspielst.',
        'Segue la tua partita mentre giochi.', 'プレイを進めると、それに合わせて更新されます。'),
    'Load story save': L('Cargar partida de la historia', 'Carregar jogo salvo da história',
                         'Charger la sauvegarde de l’histoire', 'Story-Spielstand laden',
                         'Carica il salvataggio della storia', 'ストーリーセーブをロード'),
    'Make this my story save': L('Usar esta como partida de la historia', 'Tornar este o jogo salvo da história',
                                 'En faire ma sauvegarde de l’histoire', 'Zu meinem Story-Spielstand machen',
                                 'Rendilo il salvataggio della storia', 'これをストーリーセーブにする'),
    'Use this after skipping ahead on purpose: the story save then follows your playthrough from where you are now.': L(
        'Úsalo después de saltar adelante a propósito: la partida de la historia seguirá tu partida desde donde estás '
        'ahora.',
        'Use isto depois de avançar de propósito: o jogo salvo da história passa a acompanhar sua jornada a partir de '
        'onde você está agora.',
        'À utiliser après avoir avancé exprès : la sauvegarde de l’histoire suit alors votre progression à partir '
        'd’où vous êtes.',
        'Nutze das, nachdem du absichtlich vorgesprungen bist: Der Story-Spielstand folgt deinem Durchgang dann ab '
        'hier.',
        'Usalo dopo essere andato avanti di proposito: il salvataggio della storia seguirà la tua partita da dove sei '
        'ora.',
        '意図的に先へ進んだ後に使います。ストーリーセーブは今いる場所から、プレイに合わせて更新されます。'),
    'Slots': L('Ranuras', 'Slots', 'Emplacements', 'Plätze', 'Slot', 'スロット'),
    'Save here': L('Guardar aquí', 'Salvar aqui', 'Sauvegarder ici', 'Hier speichern', 'Salva qui', 'ここにセーブ'),
    'Load': L('Cargar', 'Carregar', 'Charger', 'Laden', 'Carica', 'ロード'),
    'empty': L('vacía', 'vazio', 'vide', 'leer', 'vuoto', '空き'),
    'Before the last load: %s%s': L('Antes de la última carga: %s%s', 'Antes do último carregamento: %s%s',
                                    'Avant le dernier chargement : %s%s', 'Vor dem letzten Laden: %s%s',
                                    "Prima dell'ultimo caricamento: %s%s", '前回のロード前: %s%s'),
    'Replace what slot {} holds with your current save?': L(
        '¿Reemplazar lo que contiene la ranura {} por tu partida actual?',
        'Substituir o conteúdo do slot {} pelo seu jogo salvo atual?',
        'Remplacer le contenu de l’emplacement {} par votre sauvegarde actuelle ?',
        'Den Inhalt von Platz {} durch deinen aktuellen Spielstand ersetzen?',
        'Sostituire il contenuto dello slot {} con il tuo salvataggio attuale?',
        'スロット {} の内容を現在のセーブデータで上書きしますか？'),
    'Load the story save? The game restarts and Continue starts there. Your current save is kept as "Before the last '
    'load".': L(
        '¿Cargar la partida de la historia? El juego se reinicia y Continuar empieza ahí. Tu partida actual se '
        'conserva como “Antes de la última carga”.',
        'Carregar o jogo salvo da história? O jogo reinicia e Continuar começa ali. Seu jogo salvo atual fica como '
        '“Antes do último carregamento”.',
        'Charger la sauvegarde de l’histoire ? Le jeu redémarre et Continuer reprend là. Votre sauvegarde actuelle est '
        'conservée sous « Avant le dernier chargement ».',
        'Story-Spielstand laden? Das Spiel startet neu und Fortsetzen beginnt dort. Dein aktueller Spielstand bleibt '
        'als „Vor dem letzten Laden“ erhalten.',
        "Caricare il salvataggio della storia? Il gioco si riavvia e Continua parte da lì. Il salvataggio attuale "
        "resta come “Prima dell'ultimo caricamento”.",
        'ストーリーセーブをロードしますか？ゲームが再起動し、コンティニューはそこから始まります。'
        '現在のセーブデータは「前回のロード前」として残ります。'),
    'Load the save from before the last load? The game restarts and Continue starts there. Your current save is kept '
    'as "Before the last load".': L(
        '¿Cargar la partida de antes de la última carga? El juego se reinicia y Continuar empieza ahí. Tu partida '
        'actual se conserva como “Antes de la última carga”.',
        'Carregar o jogo salvo de antes do último carregamento? O jogo reinicia e Continuar começa ali. Seu jogo salvo '
        'atual fica como “Antes do último carregamento”.',
        'Charger la sauvegarde d’avant le dernier chargement ? Le jeu redémarre et Continuer reprend là. Votre '
        'sauvegarde actuelle est conservée sous « Avant le dernier chargement ».',
        'Den Spielstand von vor dem letzten Laden laden? Das Spiel startet neu und Fortsetzen beginnt dort. Dein '
        'aktueller Spielstand bleibt als „Vor dem letzten Laden“ erhalten.',
        "Caricare il salvataggio di prima dell'ultimo caricamento? Il gioco si riavvia e Continua parte da lì. Il "
        "salvataggio attuale resta come “Prima dell'ultimo caricamento”.",
        '前回のロード前のセーブデータをロードしますか？ゲームが再起動し、コンティニューはそこから始まります。'
        '現在のセーブデータは「前回のロード前」として残ります。'),
    'Load slot {}? The game restarts and Continue starts there. Your current save is kept as "Before the last '
    'load".': L(
        '¿Cargar la ranura {}? El juego se reinicia y Continuar empieza ahí. Tu partida actual se conserva como '
        '“Antes de la última carga”.',
        'Carregar o slot {}? O jogo reinicia e Continuar começa ali. Seu jogo salvo atual fica como “Antes do último '
        'carregamento”.',
        'Charger l’emplacement {} ? Le jeu redémarre et Continuer reprend là. Votre sauvegarde actuelle est conservée '
        'sous « Avant le dernier chargement ».',
        'Platz {} laden? Das Spiel startet neu und Fortsetzen beginnt dort. Dein aktueller Spielstand bleibt als „Vor '
        'dem letzten Laden“ erhalten.',
        "Caricare lo slot {}? Il gioco si riavvia e Continua parte da lì. Il salvataggio attuale resta come “Prima "
        "dell'ultimo caricamento”.",
        'スロット {} をロードしますか？ゲームが再起動し、コンティニューはそこから始まります。'
        '現在のセーブデータは「前回のロード前」として残ります。'),
    'Make your current save the story save?': L(
        '¿Convertir tu partida actual en la partida de la historia?',
        'Tornar seu jogo salvo atual o jogo salvo da história?',
        'Faire de votre sauvegarde actuelle la sauvegarde de l’histoire ?',
        'Deinen aktuellen Spielstand zum Story-Spielstand machen?',
        'Rendere il salvataggio attuale il salvataggio della storia?',
        '現在のセーブデータをストーリーセーブにしますか？'),
    "Copies of this game's campaign save (the one Continue loads). Treasures, unlocks and trophies are not "
    "changed.": L(
        'Copias de la partida de la campaña de este juego (la que carga Continuar). Los tesoros, desbloqueos y trofeos '
        'no cambian.',
        'Cópias do jogo salvo da campanha deste jogo (o que Continuar carrega). Tesouros, desbloqueios e troféus não '
        'são alterados.',
        'Copies de la sauvegarde de campagne de ce jeu (celle que charge Continuer). Les trésors, déblocages et '
        'trophées ne sont pas modifiés.',
        'Kopien des Kampagnen-Spielstands dieses Spiels (den Fortsetzen lädt). Schätze, Freischaltungen und Trophäen '
        'werden nicht verändert.',
        'Copie del salvataggio della campagna di questo gioco (quello caricato da Continua). Tesori, sbloccabili e '
        'trofei non vengono modificati.',
        'このゲームのキャンペーンのセーブデータ(コンティニューでロードされるもの)のコピーです。宝物、'
        'アンロック要素、トロフィーは変更されません。'),

    # ---- Messages (bbport_saves.cpp) and checkpoint descriptions ---------------------------------------------------
    'Save slots are only for Uncharted 1, 2 and 3.': L(
        'Las ranuras de guardado son solo para Uncharted 1, 2 y 3.',
        'Os slots de salvamento são só para Uncharted 1, 2 e 3.',
        'Les emplacements de sauvegarde ne concernent qu’Uncharted 1, 2 et 3.',
        'Speicherplätze gibt es nur für Uncharted 1, 2 und 3.',
        'Gli slot di salvataggio sono solo per Uncharted 1, 2 e 3.',
        'セーブスロットは Uncharted 1、2、3 専用です。'),
    'The game is saving: try again in a moment.': L(
        'El juego está guardando: inténtalo de nuevo en un momento.', 'O jogo está salvando: tente de novo em instantes.',
        'Le jeu sauvegarde : réessayez dans un instant.', 'Das Spiel speichert gerade: Versuch es gleich noch einmal.',
        'Il gioco sta salvando: riprova tra un momento.', 'ゲームがセーブ中です。少し待ってからもう一度お試しください。'),
    'There is no campaign save yet.': L(
        'Todavía no hay partida de la campaña.', 'Ainda não há jogo salvo da campanha.',
        'Il n’y a pas encore de sauvegarde de campagne.', 'Es gibt noch keinen Kampagnen-Spielstand.',
        "Non c'è ancora un salvataggio della campagna.", 'キャンペーンのセーブデータがまだありません。'),
    'Could not copy the save: {}': L(
        'No se pudo copiar la partida: {}', 'Não foi possível copiar o jogo salvo: {}',
        'Impossible de copier la sauvegarde : {}', 'Der Spielstand konnte nicht kopiert werden: {}',
        'Impossibile copiare il salvataggio: {}', 'セーブデータをコピーできませんでした: {}'),
    'Saved in slot {}.': L('Guardado en la ranura {}.', 'Salvo no slot {}.', 'Sauvegardé dans l’emplacement {}.',
                           'In Platz {} gespeichert.', 'Salvato nello slot {}.', 'スロット {} にセーブしました。'),
    'Saved.': L('Guardado.', 'Salvo.', 'Sauvegardé.', 'Gespeichert.', 'Salvato.', 'セーブしました。'),
    'This is now your story save.': L(
        'Ahora esta es tu partida de la historia.', 'Este agora é o seu jogo salvo da história.',
        'C’est maintenant votre sauvegarde de l’histoire.', 'Das ist jetzt dein Story-Spielstand.',
        'Ora questo è il tuo salvataggio della storia.', 'これがストーリーセーブになりました。'),
    'That slot is empty.': L('Esa ranura está vacía.', 'Esse slot está vazio.', 'Cet emplacement est vide.',
                             'Dieser Platz ist leer.', 'Quello slot è vuoto.', 'そのスロットは空です。'),
    'Could not write the load request.': L(
        'No se pudo escribir la solicitud de carga.', 'Não foi possível gravar o pedido de carregamento.',
        'Impossible d’écrire la demande de chargement.', 'Die Ladeanforderung konnte nicht geschrieben werden.',
        'Impossibile scrivere la richiesta di caricamento.', 'ロードの要求を書き込めませんでした。'),
    'Practice mode is only for Uncharted 1, 2 and 3.': L(
        'El modo práctica es solo para Uncharted 1, 2 y 3.', 'O modo de treino é só para Uncharted 1, 2 e 3.',
        'Le mode entraînement ne concerne qu’Uncharted 1, 2 et 3.', 'Den Übungsmodus gibt es nur für Uncharted 1, 2 und 3.',
        'La modalità pratica è solo per Uncharted 1, 2 e 3.', '練習モードは Uncharted 1、2、3 専用です。'),
    'Could not start practice mode: {}': L(
        'No se pudo iniciar el modo práctica: {}', 'Não foi possível iniciar o modo de treino: {}',
        'Impossible de lancer le mode entraînement : {}', 'Der Übungsmodus konnte nicht gestartet werden: {}',
        'Impossibile avviare la modalità pratica: {}', '練習モードを開始できませんでした: {}'),
    'Going to {}...': L('Yendo a {}...', 'Indo para {}...', 'Direction {}...', 'Springe zu {}...', 'Vado a {}...',
                        '{} へ移動中...'),
    'Nothing to save yet: the game has not saved since the jump.': L(
        'Aún no hay nada que guardar: el juego no ha guardado desde el salto.',
        'Ainda não há nada para salvar: o jogo não salvou desde o salto.',
        'Rien à sauvegarder pour l’instant : le jeu n’a pas sauvegardé depuis le saut.',
        'Noch nichts zu speichern: Das Spiel hat seit dem Sprung nicht gespeichert.',
        'Ancora niente da salvare: il gioco non ha salvato dopo il salto.',
        'まだセーブするものがありません。ジャンプ後にゲームがセーブしていません。'),
    'Nothing to keep yet: the game has not saved since the jump.': L(
        'Aún no hay nada que conservar: el juego no ha guardado desde el salto.',
        'Ainda não há nada para manter: o jogo não salvou desde o salto.',
        'Rien à garder pour l’instant : le jeu n’a pas sauvegardé depuis le saut.',
        'Noch nichts zu behalten: Das Spiel hat seit dem Sprung nicht gespeichert.',
        'Ancora niente da mantenere: il gioco non ha salvato dopo il salto.',
        'まだ残すものがありません。ジャンプ後にゲームがセーブしていません。'),
    'Could not write the request.': L(
        'No se pudo escribir la solicitud.', 'Não foi possível gravar o pedido.', 'Impossible d’écrire la demande.',
        'Die Anforderung konnte nicht geschrieben werden.', 'Impossibile scrivere la richiesta.',
        '要求を書き込めませんでした。'),
    'an unknown checkpoint': L('un punto de control desconocido', 'um checkpoint desconhecido',
                               'un point de contrôle inconnu', 'ein unbekannter Checkpoint',
                               'un checkpoint sconosciuto', '不明なチェックポイント'),
    'no save yet': L('aún no hay partida', 'ainda não há jogo salvo', 'pas encore de sauvegarde', 'noch kein Spielstand',
                     'nessun salvataggio ancora', 'セーブデータなし'),
    'Checkpoint {}': L('Punto de control {}', 'Checkpoint {}', 'Point de contrôle {}', 'Checkpoint {}',
                       'Checkpoint {}', 'チェックポイント {}'),
    'Cutscene': L('Cinemática', 'Cena', 'Cinématique', 'Zwischensequenz', 'Filmato', 'カットシーン'),
    'Chapter {}': L('Capítulo {}', 'Capítulo {}', 'Chapitre {}', 'Kapitel {}', 'Capitolo {}', '第{}章'),

    # ---- Text entry, trophy notice ---------------------------------------------------------------------------------
    'Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel': L(
        'Teclado: escribe, Retroceso para borrar, Enter = aceptar, Esc = cancelar',
        'Teclado: digite, Backspace para apagar, Enter = OK, Esc = cancelar',
        'Clavier : tapez, Retour arrière pour effacer, Entrée = OK, Échap = annuler',
        'Tastatur: tippen, Rücktaste zum Löschen, Enter = OK, Esc = abbrechen',
        'Tastiera: digita, Backspace per cancellare, Invio = OK, Esc = annulla',
        'キーボード: 入力、Backspace で削除、Enter = OK、Esc = キャンセル'),
    'Controller: Cross (A) = OK, Circle (B) = cancel': L(
        'Mando: Cruz (A) = aceptar, Círculo (B) = cancelar', 'Controle: Xis (A) = OK, Círculo (B) = cancelar',
        'Manette : Croix (A) = OK, Rond (B) = annuler', 'Controller: Kreuz (A) = OK, Kreis (B) = abbrechen',
        'Controller: Croce (A) = OK, Cerchio (B) = annulla', 'コントローラー: ×ボタン (A) = OK、○ボタン (B) = キャンセル'),
    'Platinum trophy earned': L('Trofeo de platino obtenido', 'Troféu de platina conquistado',
                                'Trophée de platine obtenu', 'Platin-Trophäe erhalten', 'Trofeo di platino ottenuto',
                                'プラチナトロフィーを獲得しました'),
    'Gold trophy earned': L('Trofeo de oro obtenido', 'Troféu de ouro conquistado', 'Trophée d’or obtenu',
                            'Gold-Trophäe erhalten', "Trofeo d'oro ottenuto", 'ゴールドトロフィーを獲得しました'),
    'Silver trophy earned': L('Trofeo de plata obtenido', 'Troféu de prata conquistado', 'Trophée d’argent obtenu',
                              'Silber-Trophäe erhalten', "Trofeo d'argento ottenuto", 'シルバートロフィーを獲得しました'),
    'Bronze trophy earned': L('Trofeo de bronce obtenido', 'Troféu de bronze conquistado', 'Trophée de bronze obtenu',
                              'Bronze-Trophäe erhalten', 'Trofeo di bronzo ottenuto', 'ブロンズトロフィーを獲得しました'),

    # ---- Shader preparation ----------------------------------------------------------------------------------------
    'Preparing shaders': L('Preparando shaders', 'Preparando shaders', 'Préparation des shaders',
                           'Shader werden vorbereitet', 'Preparazione degli shader', 'シェーダーを準備中'),
    'Preparing pipelines': L('Preparando pipelines', 'Preparando pipelines', 'Préparation des pipelines',
                             'Pipelines werden vorbereitet', 'Preparazione delle pipeline', 'パイプラインを準備中'),
    'Preparing shaders %u / %u%s': L('Preparando shaders %u / %u%s', 'Preparando shaders %u / %u%s',
                                     'Préparation des shaders %u / %u%s', 'Shader werden vorbereitet %u / %u%s',
                                     'Preparazione degli shader %u / %u%s', 'シェーダーを準備中 %u / %u%s'),
    'Preparing pipelines %u / %u%s': L('Preparando pipelines %u / %u%s', 'Preparando pipelines %u / %u%s',
                                       'Préparation des pipelines %u / %u%s', 'Pipelines werden vorbereitet %u / %u%s',
                                       'Preparazione delle pipeline %u / %u%s', 'パイプラインを準備中 %u / %u%s'),
    'Your graphics driver is compiling the shaders again (after a driver update or when its cache was cleared). The '
    'game continues when this is done.': L(
        'El driver gráfico está compilando los shaders de nuevo (tras actualizarlo o borrar su caché). El juego '
        'continúa al terminar.',
        'O driver gráfico está compilando os shaders de novo (após uma atualização ou com o cache dele apagado). O '
        'jogo continua quando terminar.',
        'Le pilote graphique compile à nouveau les shaders (après une mise à jour ou un cache vidé). Le jeu reprend '
        'une fois terminé.',
        'Der Grafiktreiber kompiliert die Shader erneut (nach einem Treiberupdate oder geleertem Cache). Das Spiel geht '
        'danach weiter.',
        'Il driver grafico sta compilando di nuovo gli shader (dopo un aggiornamento o con la sua cache svuotata). Il '
        'gioco riprende al termine.',
        'グラフィックドライバーがシェーダーを再びコンパイルしています（ドライバーの更新後やキャッシュの削除後）。完了するとゲームが続きます。'),
    'This happens once, before your first launch. Later launches start straight away.': L(
        'Esto ocurre una vez, antes del primer inicio. Los siguientes inicios empiezan al momento.',
        'Isso acontece uma vez, antes da primeira inicialização. As próximas começam na hora.',
        'Cela se fait une fois, avant le premier lancement. Les lancements suivants démarrent immédiatement.',
        'Das passiert einmal, vor dem ersten Start. Spätere Starts beginnen sofort.',
        'Succede una volta, prima del primo avvio. Gli avvii successivi partono subito.',
        'これは初回起動前に一度だけ行われます。次回以降はすぐに始まります。'),
    'Step 1 of 2: finding the shaders in your game files (only this once)': L(
        'Paso 1 de 2: buscando los shaders en los archivos del juego (solo esta vez)',
        'Etapa 1 de 2: procurando os shaders nos arquivos do jogo (só desta vez)',
        'Étape 1 sur 2 : recherche des shaders dans les fichiers du jeu (cette fois seulement)',
        'Schritt 1 von 2: Shader in deinen Spieldateien suchen (nur dieses eine Mal)',
        'Passo 1 di 2: ricerca degli shader nei file del gioco (solo questa volta)',
        '手順 1/2: ゲームファイルからシェーダーを検索中(初回のみ)'),
    'Step 1 of 2: loading shaders': L(
        'Paso 1 de 2: cargando los shaders', 'Etapa 1 de 2: carregando os shaders',
        'Étape 1 sur 2 : chargement des shaders', 'Schritt 1 von 2: Shader werden geladen',
        'Passo 1 di 2: caricamento degli shader', '手順 1/2: シェーダーを読み込み中'),
    'Step 1 of 2: building shaders from your game files': L(
        'Paso 1 de 2: creando los shaders a partir de los archivos del juego',
        'Etapa 1 de 2: criando os shaders a partir dos arquivos do jogo',
        'Étape 1 sur 2 : création des shaders à partir des fichiers du jeu',
        'Schritt 1 von 2: Shader aus deinen Spieldateien erstellen',
        'Passo 1 di 2: creazione degli shader dai file del gioco',
        '手順 1/2: ゲームファイルからシェーダーを作成中'),
    'Step 2 of 2: compiling them for your graphics card': L(
        'Paso 2 de 2: compilándolos para tu tarjeta gráfica', 'Etapa 2 de 2: compilando-os para sua placa de vídeo',
        'Étape 2 sur 2 : compilation pour votre carte graphique', 'Schritt 2 von 2: Kompilieren für deine Grafikkarte',
        'Passo 2 di 2: compilazione per la tua scheda video', '手順 2/2: グラフィックカード向けにコンパイル中'),
    'Finishing...': L('Finalizando...', 'Finalizando...', 'Finalisation...', 'Wird abgeschlossen...',
                      'Completamento...', '完了処理中...'),
    'Starting...': L('Iniciando...', 'Iniciando...', 'Démarrage...', 'Wird gestartet...', 'Avvio...', '開始中...'),
    # The next two are one sentence over two centred lines.
    'If a large shader cache causes problems: in the launcher, clear the shader cache and turn off': L(
        'Si una caché de shaders grande da problemas: en el launcher, borra la caché de shaders y desactiva',
        'Se um cache de shaders grande causar problemas: no launcher, limpe o cache de shaders e desligue',
        'Si un gros cache des shaders pose problème : dans le launcher, videz le cache des shaders et désactivez',
        'Wenn ein großer Shader-Cache Probleme macht: Leere im Launcher den Shader-Cache und schalte',
        'Se una cache degli shader grande causa problemi: nel launcher svuota la cache degli shader e disattiva',
        '大きなシェーダーキャッシュで問題が起きる場合: ランチャーでシェーダーキャッシュを消去し、次をオフにしてください:'),
    'Advanced > Shader preparation.': L(
        'Avanzado > Preparación de shaders.', 'Avançado > Preparação de shaders.', 'Avancé > Préparation des shaders.',
        'Erweitert > Shader-Vorbereitung aus.', 'Avanzate > Preparazione degli shader.', '詳細設定 > シェーダーの準備。'),
    'Finding the shaders in your game files (only this once) %.1f / %.1f GB%s': L(
        'Buscando los shaders en los archivos del juego (solo esta vez) %.1f / %.1f GB%s',
        'Procurando os shaders nos arquivos do jogo (só desta vez) %.1f / %.1f GB%s',
        'Recherche des shaders dans les fichiers du jeu (cette fois seulement) %.1f / %.1f Go%s',
        'Shader in deinen Spieldateien suchen (nur dieses eine Mal) %.1f / %.1f GB%s',
        'Ricerca degli shader nei file del gioco (solo questa volta) %.1f / %.1f GB%s',
        'ゲームファイルからシェーダーを検索中(初回のみ) %.1f / %.1f GB%s'),
    # Time left (also sent to the launcher's bar, which shows it as it comes).
    'about {:.0f} min left': L('quedan unos {:.0f} min', 'faltam cerca de {:.0f} min', 'environ {:.0f} min restantes',
                               'noch etwa {:.0f} min', 'circa {:.0f} min rimanenti', '残り約 {:.0f} 分'),
    'about {:.0f} s left': L('quedan unos {:.0f} s', 'faltam cerca de {:.0f} s', 'environ {:.0f} s restantes',
                             'noch etwa {:.0f} s', 'circa {:.0f} s rimanenti', '残り約 {:.0f} 秒'),
    'about {} min {} s left': L('quedan unos {} min {} s', 'faltam cerca de {} min {} s',
                                'environ {} min {} s restantes', 'noch etwa {} min {} s', 'circa {} min {} s rimanenti',
                                '残り約 {} 分 {} 秒'),
    'about {} s left': L('quedan unos {} s', 'faltam cerca de {} s', 'environ {} s restantes', 'noch etwa {} s',
                         'circa {} s rimanenti', '残り約 {} 秒'),
    'a few seconds left': L('quedan unos segundos', 'faltam alguns segundos', 'encore quelques secondes',
                            'noch wenige Sekunden', 'mancano pochi secondi', '残り数秒'),
}
