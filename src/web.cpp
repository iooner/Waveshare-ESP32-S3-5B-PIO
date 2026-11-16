#include "web.h"
#include "air.h"
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include "astro.h"
#include "agenda.h"
#include "brightness.h"
#include "crypto.h"
#include "stocks.h"
#include "wiki.h"
#include "gfx.h"
#include "lcd.h"
#include "plugin.h"
#include "space.h"
#include "weather.h"

#define BUF_SIZE           40960   // une requête : celle des réglages fait ~25 Ko avec 250 titres
#define PAGE_SIZE          163840  // la page : ~135 Ko avec 250 titres
#define HTTP_TIMEOUT_MS    1500  // pour recevoir une requête entière
#define IDLE_TIMEOUT_MS    600   // pour en recevoir le début
#define UPDATE_TIMEOUT_MS  10000  // silence maximal pendant l'envoi d'un firmware

static char *req, *page;  // requête reçue et page envoyée, en PSRAM
static size_t page_len;
static volatile bool updating = false;

// La page ressemble à un écran de réglages : des sections arrondies, une ligne par réglage, le
// libellé à gauche et la commande à droite. Elle est rangée par page de l'écran (accueil, Sonos,
// crypto, agenda), puis les réglages communs, la maintenance et l'état. Chaque case ou curseur
// est enregistré dès qu'il change (voir SCRIPT) ; seul le lieu attend son bouton.
static const char HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Écran</title><style>body{font:17px system-ui;max-width:26em;margin:0 auto;padding:0 1em 1em;background:#000;"
    "color:#eee}h1{font-size:1.5em;margin:0;padding:.7em .4em .4em;position:sticky;top:0;background:#000}"
    "h2{font:600 .8em system-ui;color:#999;text-transform:uppercase;"
    "margin:1.8em 1.2em .5em}section{background:#1c1c1e;border-radius:12px;padding:0 1em}"
    "section>*{display:flex;align-items:center;gap:.7em;min-height:2.9em;border-top:1px solid #333}"
    "section>:first-child{border:0}input[type=checkbox]{width:1.3em;height:1.3em;margin-left:auto;accent-color:#30d158}"
    "input[type=range]{flex:1;min-width:0;accent-color:#0a84ff}input:not([type]),button{font:inherit;color:#fff;"
    "background:#2c2c2e;border:0;border-radius:8px;padding:.45em .7em}input:not([type]){flex:1;min-width:0}input[name^=n]:not([type]){flex:0 0 5em}"
    "input[type=number]{width:4.5em;text-align:right}input[type=time],input[type=number]{font:inherit;color:#fff;"
    "background:#2c2c2e;border:0;border-radius:8px;padding:.3em .5em;margin-left:auto;color-scheme:dark}"
    "button{background:#0a84ff}#r,#cr,#ol,#on{flex-wrap:wrap;padding:.5em 0}#r:empty,#cr:empty,#sm:empty{display:none}"
    "#r button,#cr button,#on button,label button{background:#2c2c2e}#ol button{background:#30d158;color:#000}"
    "textarea{font:inherit;color:#fff;background:#2c2c2e;border:0;border-radius:8px;padding:.45em .7em;flex:1;min-width:0;height:2.2em}"
    "input[type=file]{font:inherit;color:#999;min-width:0}b{margin-left:auto;font-weight:400;color:#999;text-align:right}"
    "small{font-size:.6em;font-weight:400;color:#999}p{font-size:.8em;color:#999;margin:.5em 1.2em}"
    "a{color:#0a84ff;text-decoration:none}footer{text-align:center;color:#999;font-size:.8em;line-height:1.9;"
    "margin:3em 0 1.5em}</style><h1>Écran <small id=ok></small></h1><form>";

// Diaporama. L'ordre de passage se compose en cliquant : les pastilles du haut sont l'ordre en
// place (un clic en retire une), les boutons du dessous ajoutent une page à la suite.
// Argument : l'ordre en place, tel qu'il est envoyé à la carte.
static const char SHOW_HEAD[] =
    "<h2>Diaporama</h2><section><div id=ol></div><div id=on>";
static const char SHOW_ADD[] = "<button type=button data-n=%s onclick=oa(dataset.n)>+ %s</button>";  // nom de la page, puis son libellé
static const char SHOW_ORDER[] = "</div><input type=hidden name=ordre value='%s'>";
// Arguments : nom (initiale, suite), rang, puis la durée de son tour ou cochée ou non
static const char PAGE_SECONDS[] = "<label>%c%s pendant<input type=number name=t%u min=5 max=3600 value=%u>s</label>";
static const char PAGE_EXCLUSIVE[] = "<label>%c%s prioritaire<input type=checkbox name=x%u%s></label>";
// Arguments : fondu coché ou non, puis météo, soleil, lune, espace et particules cochés ou non,
// latitude, longitude
static const char SHOW_TAIL[] =
    "<label>Fondu entre les pages<input type=checkbox name=fondu%s></label></section>"
    "<p>En haut, l'ordre de passage : cliquer sur une page du dessous l'ajoute à la suite, cliquer sur une pastille "
    "la retire. Une page peut revenir plusieurs fois ; une page absente ne passe pas. Vide, toutes les pages passent."
    "<p>Une page sans rien à montrer est sautée : Sonos quand rien ne joue, la crypto sans crypto choisie, un agenda "
    "vide, l'air sans capteur. Sonos prioritaire garde l'écran pour elle tant qu'une enceinte joue."
    "<h2>Accueil</h2><section>"
    "<label>Météo<input type=checkbox name=meteo%s></label>"
    "<label>Lever et coucher du soleil<input type=checkbox name=soleil%s></label>"
    "<label>Phase de la lune<input type=checkbox name=lune%s></label>"
    "<label>Personnes dans l'espace<input type=checkbox name=espace%s></label>"
    "<label>Particules fines<input type=checkbox name=air%s></label></section>"
    "<p>Ce que la page d'accueil affiche autour de l'heure et de la date."
    "<h2>Lieu de la météo et de l'heure</h2><section>"
    "<div><input id=q placeholder='Chercher une ville'><button type=button onclick=s()>Chercher</button></div>"
    "<div id=r></div><label>Latitude<input id=lat name=lat value=%.4f></label>"
    "<label>Longitude<input id=lon name=lon value=%.4f></label>"
    "<div><button>Enregistrer le lieu</button></div></section>"
    "<h2>Cryptomonnaies</h2><section>"
    "<div><input id=cq placeholder='Chercher une crypto'><button type=button onclick=cs()>Chercher</button></div>"
    "<div id=cr></div>";

// Une crypto choisie. Arguments : symbole, rang, quantité, rang, identifiant, rang, symbole, rang
static const char COIN_ROW[] =
    "<label>%s<input name=q%u placeholder='quantité' inputmode=decimal value='%s'><input type=hidden name=c%u value='%s'>"
    "<input type=hidden name=s%u value='%s'><button type=button onclick=cd(%u)>Retirer</button></label>";

static const char COINS_NOTE[] =
    "</section><p>Sans quantité, la page crypto affiche le cours et ses variations sur 1 heure, 24 heures et "
    "7 jours. Avec une quantité, elle affiche aussi ce qu'elle vaut, et le total du portefeuille. Les quantités "
    "restent sur la carte. Cours en euros, fournis par CoinGecko, à titre purement informatif : ils peuvent être "
    "en retard ou inexacts.<h2>Actions et ETF</h2><section>"
    "<div><input id=si placeholder='ISIN'><input id=sn placeholder='nom' maxlength=15>"
    "<button type=button onclick=sa()>Ajouter</button></div>"
    "<div><input type=file id=ip accept='.pdf,application/pdf'><button type=button onclick=ipdf()>Importer le PDF</button></div>"
    "<div><textarea id=ix placeholder='ou coller le texte du relevé'></textarea>"
    "<button type=button onclick=im()>Importer</button></div><div id=sm></div>";

// Un titre choisi. Arguments : rang, nom donné par la bourse ou ISIN, nom choisi, rang, quantité,
// rang, prix d'achat, rang, ISIN, rang
static const char STOCK_ROW[] =
    "<label><input name=kn%u placeholder='%s' value='%s' maxlength=15><input name=kq%u placeholder='quantité' "
    "inputmode=decimal value='%s'><input name=kp%u placeholder=\"prix d'achat\" inputmode=decimal value='%s'>"
    "<input type=hidden name=ki%u value='%s'><button type=button onclick=sd(%u)>×</button></label>";

static const char STOCKS_NOTE[] =
    "</section><p>Un titre se désigne par son ISIN, affiché dans Trade Republic sous le nom de l'action ou de l'ETF "
    "(par exemple US1912161007 pour Coca-Cola). 250 titres au plus ; au-delà de six, la page les fait défiler par groupes, du plus gros au plus petit. Sans quantité, la page actions affiche le cours et "
    "sa variation du jour ; avec une quantité, ce qu'il vaut et le total du portefeuille ; avec le prix d'achat "
    "moyen d'une part, la plus-value. Pour tout mettre à jour d'un coup : dans Trade Republic, télécharger l'État du patrimoine et l'importer ici (ou coller son texte). Le PDF est lu par le navigateur, avec pdf.js ; la carte ne reçoit que les quantités. Quantités et prix restent sur la carte. Cours en euros de la bourse Lang & "
    "Schwarz, celle de Trade Republic, à titre purement informatif : ils peuvent être en retard ou inexacts."
    "<h2>Mes agendas</h2><section>";

// Un agenda personnel : son nom, puis son adresse. L'adresse n'est jamais renvoyée au navigateur :
// son champ reste vide, son texte grisé dit s'il y en a une. Arguments : rang (à partir de 1),
// nom, rang, texte grisé, rang.
static const char URL_ROW[] =
    "<label><input name=n%u placeholder='nom' value='%s' maxlength=20><input name=u%u placeholder='%s' autocomplete=off>"
    "<button type=button onclick=ud(%u)>Retirer</button></label>";

// Arguments : adresse du capteur, niveau dans la barre coché ou non, luminosité de jour (deux
// fois), cycle coché ou non, luminosité de nuit (deux fois), rouge coché ou non, extinction cochée
// ou non, heures et minutes d'extinction puis de rallumage, veille profonde cochée ou non, rang de
// la mire, cochée ou non
static const char SETTINGS[] =
    "</section><p>Les prochains événements de vos agendas, fondus en une liste ; le nom donné à un agenda est "
    "affiché avec ses événements. Pour un agenda Google : Paramètres de l'agenda, Intégrer l'agenda, puis copier "
    "son adresse au format iCal ici. L'adresse publique suffit pour un agenda public ; pour un agenda privé, il "
    "faut l'adresse secrète."
    "<p>Une adresse secrète donne accès à tout l'agenda. Elle est gardée sur la carte et n'est plus affichée ici "
    "une fois enregistrée. L'agenda du Liège Hackerspace, lui, n'a rien à régler."
    "<h2>Capteur d'air</h2><section>"
    "<label>Adresse<input name=airhost placeholder='adresse du capteur' value='%s'></label>"
    "<label>Niveau dans la barre du haut<input type=checkbox name=airbar%s></label></section>"
    "<p>Un capteur Sensor.Community du réseau local. Ses particules fines (PM2,5 et PM10, en µg/m³) s'affichent en "
    "petit sur l'accueil et en grand sur la page air, avec leur niveau sur les dix de l'indice belge BelAQI. La "
    "barre du haut des autres pages peut en donner le niveau en un mot : la moyenne des deux."
    "<h2>Luminosité</h2><section>"
    "<label>Jour<input type=range name=jour min=1 max=100 value=%u><b>%u %%</b></label>"
    "<label>Cycle automatique<input type=checkbox name=auto%s></label>"
    "<label>Nuit<input type=range name=nuit min=0 max=100 value=%u><b>%u %%</b></label>"
    "<label>Rouge sombre la nuit<input type=checkbox name=rouge%s></label></section>"
    "<p>Le cycle passe du niveau de jour au niveau de nuit quand le soleil se couche. Sans lui, seul le niveau de "
    "jour sert. Le rouge suit le soleil de la même façon, avec ou sans le cycle."
    "<h2>Extinction</h2><section>"
    "<label>Extinction programmée<input type=checkbox name=veille%s></label>"
    "<label>Éteindre à<input type=time name=debut value=%02u:%02u></label>"
    "<label>Rallumer à<input type=time name=fin value=%02u:%02u></label>"
    "<label>Veille profonde pendant l'extinction<input type=checkbox name=profonde%s></label></section>"
    "<p>L'écran s'éteint tout à fait entre les deux heures, chaque jour. En veille profonde, la carte s'arrête "
    "aussi pour consommer moins : cette page ne répond plus jusqu'à l'heure du rallumage. Pour la joindre avant, "
    "appuyer sur son bouton reset : elle reste éveillée 5 minutes."
    "<h2>Maintenance</h2><section><label>Mire de test<input type=checkbox name=p%u%s></label>"
    "<div><input type=file id=f accept=.bin></div>"
    "<div><button type=button onclick=u()>Envoyer le firmware</button><span id=m></span></div>"
    "<div><button type=button onclick=rb()>Redémarrer la carte</button></div></section></form>";

static const char FOOTER[] =
    "<footer><a href=https://github.com/iooner/Waveshare-ESP32-S3-5B-PIO>Code source sur GitHub</a><br>"
    "Crafted with love in 🇧🇪 by <a href=https://iooner.io>iooner.io</a></footer>";

// Arguments : page affichée, heure locale, soleil et lune, heures et minutes de fonctionnement, cause du
// démarrage, signal Wi-Fi,
// RAM interne libre et son minimum, images ratées, morceaux en retard et plus longue copie, luminosité,
// (agenda puis personnes dans l'espace, -1 si inconnu, après la crypto) teinte de nuit, météo, crypto, partition,
// marge des piles web, météo, Sonos et crypto
static const char STATE[] =
    "<h2>État</h2><section><div>Page affichée<b>%s</b></div><div>Heure locale<b>%02d:%02d</b></div><div>Soleil et lune<b>%s</b></div>"
    "<div>Allumé depuis<b>%lu h %02lu min</b></div><div>Dernier démarrage<b>%s</b></div><div>Wi-Fi<b>%d dBm</b></div>"
    "<div>RAM interne libre<b>%u Ko, au plus bas %u Ko</b></div><div>Images ratées<b>%lu</b></div><div>Balayage<b>%lu retards, copie max %lu µs, palettes libres %u</b></div>"
    "<div>Luminosité<b>%u %%, rouge %u/%u</b></div><div>Météo<b>%s</b></div><div>Crypto<b>%s</b></div><div>Actions<b>%s</b></div><div>Wikipédia<b>%s</b></div><div>Agendas<b>%s</b></div><div>Dans l'espace<b>%d</b></div><div>Particules<b>%s</b></div><div>Firmware<b>partition %s</b></div>"
    "<div>Marge des piles<b>web %u, météo %u, Sonos %u, crypto %u, actions %u, wiki %u, agenda %u</b></div></section>";

static const char SCRIPT[] =
    "<script>let F=document.forms[0];"
    // Ordre du diaporama : O en est la liste ; chaque changement la redessine et l'enregistre
    "let O=F.ordre.value.split(',').map(x=>x.trim()).filter(x=>x);"
    "function od(){ol.textContent=O.length?'':'toutes les pages, dans l\\'ordre du dessous';O.forEach((n,i)=>{"
    "let b=document.createElement('button');b.type='button';let a=on.querySelector('[data-n='+n+']');b.textContent=(i+1)+'. '+(a?a.textContent.slice(2):n)+' ×';"
    "b.onclick=()=>{O.splice(i,1);os()};ol.append(b)})}"
    "function os(){F.ordre.value=O.join(',');od();save()}function oa(n){if(O.length<16){O.push(n);os()}}"
    // Tout réglage part dès qu'il change, sans recharger la page. Le lieu attend son bouton : ses
    // deux champs n'ont de sens qu'ensemble.
    "async function save(){ok.textContent='...';try{let a=await fetch('/set?'+new URLSearchParams(new FormData(F)));"
    "ok.textContent=a.ok?'enregistré':'refusé'}catch(e){ok.textContent='carte injoignable'}}"
    "F.onchange=e=>{let t=e.target;if(t.type=='file')return;if(t.name[0]=='u')save().then(()=>location.reload());else if(t.type!='text'||'qnak'.includes(t.name[0]))save()};F.onsubmit=e=>{e.preventDefault();save()};"
    "F.oninput=e=>{if(e.target.type=='range')e.target.nextElementSibling.textContent=e.target.value+' %'};"
    // La recherche de ville part du navigateur, pas de la carte ; choisir une ville l'enregistre
    "q.onkeydown=e=>{if(e.key=='Enter'){e.preventDefault();s()}};async function s(){r.textContent='...';"
    "try{let j=await(await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=fr&name='+"
    "encodeURIComponent(q.value))).json();r.textContent=j.results?'':'Aucune ville trouvée';"
    "(j.results||[]).forEach(c=>{let b=document.createElement('button');b.type='button';"
    "b.textContent=[c.name,c.admin1,c.country_code].filter(x=>x).join(', ');"
    "b.onclick=()=>{lat.value=c.latitude;lon.value=c.longitude;r.textContent='';save()};r.append(b)})}"
    "catch(e){r.textContent='Recherche impossible'}}"
    // Cryptos : la recherche part aussi du navigateur. En choisir une l'ajoute à la suite, en retirer
    // une vide son identifiant ; la carte range la liste et la page se recharge pour la montrer.
    "cq.onkeydown=e=>{if(e.key=='Enter'){e.preventDefault();cs()}};async function cs(){cr.textContent='...';"
    "try{let j=await(await fetch('https://api.coingecko.com/api/v3/search?query='+encodeURIComponent(cq.value))).json();"
    "cr.textContent=j.coins.length?'':'Aucune crypto trouvée';j.coins.slice(0,6).forEach(c=>{"
    "let b=document.createElement('button');b.type='button';b.textContent=c.name+' ('+c.symbol+')';"
    "b.onclick=()=>ca(c.id,c.symbol);cr.append(b)})}catch(e){cr.textContent='Recherche impossible'}}"
    "async function ca(id,sym){let n=document.querySelectorAll('[name^=c]').length;"
    "if(n>5){cr.textContent='Six cryptos au plus';return}let h=(k,v)=>{let i=document.createElement('input');"
    "i.type='hidden';i.name=k+n;i.value=v;F.append(i)};h('c',id);"
    "h('s',sym.toUpperCase().replace(/[^A-Z0-9]/g,'').slice(0,7));await save();location.reload()}"
    "async function cd(i){F['c'+i].value='';await save();location.reload()}"
    // Actions : un ISIN saisi s'ajoute à la suite ; en retirer un vide son ISIN, comme pour les cryptos
    "si.onkeydown=e=>{if(e.key=='Enter'){e.preventDefault();sa()}};async function sa(){"
    "let v=si.value.toUpperCase().replace(/[^A-Z0-9]/g,''),n=document.querySelectorAll('[name^=ki]').length;"
    "if(!/^[A-Z]{2}[A-Z0-9]{9}[0-9]$/.test(v)){sm.textContent='ISIN invalide : deux lettres, puis dix chiffres ou lettres';return}"
    "if(n>249){sm.textContent='250 titres au plus';return}let h=(k,x)=>{let i=document.createElement('input');"
    "i.type='hidden';i.name=k+n;i.value=x;F.append(i)};h('ki',v);h('kn',sn.value);await save();location.reload()}"
    "async function sd(i){F['ki'+i].value='';await save();location.reload()}"
    // Import d'un relevé Trade Republic (État du patrimoine), copié depuis le PDF. Le texte copié
    // d'un PDF ne garde pas les lignes du tableau : la n-ième quantité va avec le n-ième ISIN. Les
    // cryptos, en fin de relevé, sont laissées de côté. Un relevé complet (il annonce son nombre de
    // positions) retire aussi les titres vendus.
    // Nom de chaque titre, pour ceux qui arrivent : le texte juste avant son « ISIN: », sans la
    // description du titre (Registered Shares...) ni ce qui l'entoure (quantités, prix, dates, pied de
    // page). Vide quand il n'y en a pas : la carte prend alors le nom donné par la bourse.
    "function nm(t){let N={},f=0;for(let m of t.matchAll(/ISIN:\\s*([A-Z]{2}[A-Z0-9]{9}\\d)/g)){let s=t.slice(f,m.i"
    "ndex);f=m.index+m[0].length;s=s.split(/ (?:Registered|Reg\\.|Actions|Inhaber|Aandelen|Namens|Shares|Act\\.)\\b"
    "/)[0].replace(/Pays d'enregistrement: \\S+|Locatie van bewaring: \\S+|Relevé de transaction en Allemagne|effec"
    "tenfactuur in Duitsland|NOM DU TITRE|NAAM EFFECT|PRIX PAR PIÈCE|KOERS PER STUK|VALEUR EN EUR|WAARDE IN EUR|Piè"
    "ces|stuks|Page \\d+ de \\d+|Pagina \\d+ van \\d+|\\([^)]*UTC[^)]*\\)|(Inscrite auprès|Directeurs Généraux|Gere"
    "gistreerd bij|Bestuurders).*?Pischke|\\S*\\d[.,:\\/-]\\d\\S*|\\b\\d{4,}\\b/g,'|');let n=s.split('|').pop().tri"
    "m().replace(/(\\s(Inc\\.?|Corp\\.?|Ltd\\.?|S\\.A\\.S?\\.?|SA|SE|AG|N\\.V\\.|NV|Holdings?|Group|Co\\.|Ét\\.Expl"
    "\\.P\\.G\\.Cl\\.|USD|\\(Acc\\)|\\(Dist\\)))+$/i,'').replace(/-SA$/,'');if(!N[m[1]])N[m[1]]=n.length>15?n.slice(0,14).trim("
    ")+'.':n}return N}"
    "async function im(){let t=ix.value.normalize().split(/PORTEFEUILLE CRYPTO|CRYPTOWALLET/)[0].replace(/\\s+/g,' '),"
    "q=[...t.matchAll(/(\\d[\\d.]*,\\d+|\\d+)\\s+(Pièces|stuks)/g)].map(m=>m[1].replace(/\\./g,'').replace(',','.')),"
    "s=[...t.matchAll(/ISIN:\\s*([A-Z]{2}[A-Z0-9]{9}\\d)/g)].map(m=>m[1]),c=t.match(/(POSITIONS|POSITIES) ?: ?(\\d+)/);"
    // Un relevé bilingue (français puis néerlandais) donne chaque titre deux fois : gardé une fois, si
    // les deux copies ont la même quantité
    "let P={},ok=s.length&&q.length==s.length;s.forEach((x,i)=>{if(x in P&&P[x]!=q[i])ok=false;P[x]=q[i]});"
    "let k=Object.keys(P).length;if(!ok||c&&+c[2]!=k){sm.textContent='Relevé non reconnu : '+q.length+' quantités pour '+s.length+' ISIN'"
    "+(c?', '+c[2]+' positions annoncées':'');return}"
    "let N=nm(t),a=0,u=0,d=0;document.querySelectorAll('[name^=ki]').forEach(x=>{"
    "if(!x.value)return;let i=x.name.slice(2);if(x.value in P){F['kq'+i].value=P[x.value];delete P[x.value];u++}else if(c){x.value='';d++}});"
    "let n=document.querySelectorAll('[name^=ki]').length;for(let x in P){if(n>249)break;let h=(k,v)=>{let i=document.createElement('input');"
    "i.type='hidden';i.name=k+n;i.value=v;F.append(i)};h('ki',x);h('kn',N[x]||'');h('kq',P[x]);n++;a++}"
    "sessionStorage.m='Relevé importé : '+u+' titres mis à jour, '+a+' ajoutés, '+d+' retirés';await save();location.reload()}"
    // Le PDF lui-même est lu par le navigateur, avec pdf.js chargé à ce moment-là : la carte ne
    // reçoit que les quantités
    "async function ipdf(){let f=ip.files[0];if(!f)return;sm.textContent='Lecture du PDF...';try{"
    "let L='https://cdnjs.cloudflare.com/ajax/libs/pdf.js/6.4.299/',p=await import(L+'pdf.min.mjs');"
    "p.GlobalWorkerOptions.workerSrc=L+'pdf.worker.min.mjs';let d=await p.getDocument({data:await f.arrayBuffer()}).promise,t='';"
    "for(let i=1;i<=d.numPages;i++)t+=(await(await d.getPage(i)).getTextContent()).items.map(x=>x.str).join(' ')+' ';"
    "ix.value=t;await im()}catch(e){sm.textContent='PDF illisible : '+e.message}}"
    "try{if(sessionStorage.m){sm.textContent=sessionStorage.m;sessionStorage.removeItem('m')}}catch(e){}"
    // Agendas : une adresse collée est enregistrée puis masquée ; « Retirer » le demande à la carte
    "async function ud(i){let h=document.createElement('input');h.type='hidden';h.name='ur'+i;h.value=1;F.append(h);"
    "await save();location.reload()}"
    // Le firmware part tel quel dans le corps de la requête ; la page se recharge après le redémarrage
    "function u(){if(!f.files[0])return;let x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=e=>m.textContent=Math.round(100*e.loaded/e.total)+' %';"
    "x.onload=()=>{m.textContent=x.responseText;setTimeout(()=>location.reload(),9000)};"
    "x.onerror=()=>m.textContent='Envoi interrompu';x.send(f.files[0])}od();"
    // Redémarrage : demandé par un POST, pour qu'un simple lien ou un préchargement ne le déclenche pas
    "async function rb(){if(!confirm('Redémarrer la carte ?'))return;m.textContent='Redémarrage...';"
    "try{await fetch('/reboot',{method:'POST'})}catch(e){}setTimeout(()=>location.reload(),8000)}</script>";

// Ajoute du texte à la page en cours de construction
static void add(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *format, ...) {
  va_list args;
  va_start(args, format);
  page_len += vsnprintf(page + page_len, PAGE_SIZE - page_len, format, args);
  va_end(args);
  page_len = min<size_t>(page_len, PAGE_SIZE - 1);
}

static void reply(WiFiClient &c, const char *status, const char *text) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
           status, (unsigned)strlen(text), text);
}

// Valeur du paramètre `name` de la requête, ou nul s'il est absent
static const char *param(const char *query, const char *name) {
  size_t len = strlen(name);
  for (const char *p = query; (p = strstr(p + 1, name)); p += len) {
    if ((p[-1] == '?' || p[-1] == '&') && p[len] == '=') return p + len + 1;
  }
  return nullptr;
}

// Minutes depuis minuit d'une heure "HH:MM", dont le deux-points arrive codé en %3A. -1 si elle
// est mal formée.
static int minuteOfDay(const char *value) {
  if (!value) return -1;
  char *end;
  long hours = strtol(value, &end, 10);
  if (end == value) return -1;
  if (*end == ':') end++;
  else if (strncasecmp(end, "%3A", 3) == 0) end += 3;
  else return -1;
  long minutes = strtol(end, nullptr, 10);
  return hours >= 0 && hours < 24 && minutes >= 0 && minutes < 60 ? hours * 60 + minutes : -1;
}

// Copie la valeur d'un paramètre tant qu'elle est faite de lettres, de chiffres et de tirets
static void word(const char *value, char *out, size_t cap) {
  size_t n = 0;
  while (value && n < cap - 1 && (isalnum((uint8_t)*value) || *value == '-' || *value == '_')) out[n++] = *value++;
  out[n] = 0;
}

// Quantité saisie à la main : la virgule peut tenir lieu de point, et arrive codée en %2C
static double quantity(const char *value) {
  char text[24];
  size_t n = 0;
  while (value && *value && *value != '&' && n < sizeof(text) - 1) {
    if (strncasecmp(value, "%2C", 3) == 0) text[n++] = '.', value += 3;
    else text[n++] = *value == ',' ? '.' : *value, value++;
  }
  text[n] = 0;
  double q = atof(text);
  return q > 0 ? q : 0;
}

// Texte à placer dans un attribut entre apostrophes : « & » devient &amp;, les guillemets et les
// chevrons des espaces
static void attribute(const char *text, char *out, size_t cap) {
  size_t n = 0;
  for (; *text && n + 5 < cap; text++) {
    if (*text == '&') memcpy(out + n, "&amp;", 5), n += 5;
    else out[n++] = strchr("'\"<>", *text) ? ' ' : *text;
  }
  out[n] = 0;
}

// Nombre saisi, réaffiché sans zéros inutiles ; vide s'il est nul
static void number(double value, char *out, size_t cap) {
  out[0] = 0;
  if (value <= 0) return;
  int n = snprintf(out, cap, "%.8f", value);
  while (n > 1 && out[n - 1] == '0') out[--n] = 0;
  if (out[n - 1] == '.') out[--n] = 0;
}

// Décode la valeur d'un paramètre (« %3A » pour deux-points...), jusqu'au paramètre suivant
static void decode(const char *value, char *out, size_t cap) {
  size_t n = 0;
  for (; value && *value && *value != '&' && n < cap - 1; value++) {
    if (*value == '%' && isxdigit((uint8_t)value[1]) && isxdigit((uint8_t)value[2])) {
      char hex[3] = {value[1], value[2], 0};
      out[n++] = strtol(hex, nullptr, 16);
      value += 2;
    } else {
      out[n++] = *value == '+' ? ' ' : *value;
    }
  }
  out[n] = 0;
}

// La page envoie tous ses réglages à chaque fois. Une case décochée n'est pas envoyée : paramètre
// absent = désactivé.
static void apply(const char *query) {
  for (uint8_t i = 0; i < pluginCount(); i++) {
    char name[8];
    PluginSlide slide;
    pluginSlide(i, slide);
    // Seule la mire de test se coche ; les autres pages passent dès qu'elles sont dans l'ordre du diaporama
    snprintf(name, sizeof(name), "p%u", i);
    slide.enabled = !pluginAt(i)->optional || param(query, name) != nullptr;
    snprintf(name, sizeof(name), "x%u", i);
    slide.exclusive = pluginAt(i)->optional ? pluginAt(i)->exclusive : param(query, name) != nullptr;
    snprintf(name, sizeof(name), "t%u", i);
    const char *seconds = param(query, name);
    if (seconds && atoi(seconds) > 0) slide.seconds = min(atoi(seconds), 3600);
    pluginSetSlide(i, slide);
  }

  pluginSetFade(param(query, "fondu") != nullptr);
  if (param(query, "ordre")) {
    char names[128];
    decode(param(query, "ordre"), names, sizeof(names));
    for (char *p = names; *p; p++) {
      if (!isalnum((uint8_t)*p) && *p != ',' && *p != ' ') *p = ' ';
    }
    pluginSetOrder(names);
  }

  // Agendas personnels : un champ vide ne change rien, l'adresse en place n'étant jamais renvoyée
  for (uint8_t u = 0; u < AGENDA_URLS; u++) {
    char name[8];
    static char url[320];  // hors de la pile ; une seule tâche passe ici
    snprintf(name, sizeof(name), "ur%u", u + 1);
    if (param(query, name)) {
      agendaSetUrl(u, "");
      agendaSetName(u, "");
      continue;
    }
    // Nom : sans les caractères qui casseraient la page quand elle le réaffiche
    snprintf(name, sizeof(name), "n%u", u + 1);
    if (param(query, name)) {
      char label[AGENDA_NAME_SIZE];
      decode(param(query, name), label, sizeof(label));
      for (char *p = label; *p; p++) {
        if (strchr("'\"<>&", *p)) *p = ' ';
      }
      agendaSetName(u, label);
    }
    snprintf(name, sizeof(name), "u%u", u + 1);
    decode(param(query, name), url, sizeof(url));
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0 || strncmp(url, "webcal://", 9) == 0) {
      agendaSetUrl(u, url);
    }
  }

  // Cryptos : la liste est rangée, sans les emplacements vidés. Absente de la requête (aucune
  // crypto à l'écran de réglages), elle n'est pas touchée.
  if (param(query, "c0")) {
    static CryptoCoin coins[CRYPTO_MAX];  // hors de la pile, comptée au plus juste ; une seule tâche passe ici
    memset(coins, 0, sizeof(coins));
    uint8_t count = 0;
    for (uint8_t i = 0; i < CRYPTO_MAX; i++) {
      char name[8];
      snprintf(name, sizeof(name), "c%u", i);
      word(param(query, name), coins[count].id, sizeof(coins[count].id));
      if (!coins[count].id[0]) continue;
      snprintf(name, sizeof(name), "s%u", i);
      word(param(query, name), coins[count].symbol, sizeof(coins[count].symbol));
      snprintf(name, sizeof(name), "q%u", i);
      coins[count].quantity = quantity(param(query, name));
      count++;
    }
    if (count < CRYPTO_MAX) memset(&coins[count], 0, sizeof(CryptoCoin));  // dernier emplacement essayé, resté vide
    cryptoConfigure(coins, count);
  }

  // Actions : rangées comme les cryptos. Un ISIN mal formé est ignoré.
  if (param(query, "ki0")) {
    static Stock *list = (Stock *)heap_caps_malloc(STOCKS_MAX * sizeof(Stock), MALLOC_CAP_SPIRAM);  // une seule tâche passe ici
    memset(list, 0, STOCKS_MAX * sizeof(Stock));
    uint8_t count = 0;
    for (uint8_t i = 0; i < STOCKS_MAX; i++) {
      char name[8];
      Stock &st = list[count];
      snprintf(name, sizeof(name), "ki%u", i);
      word(param(query, name), st.isin, sizeof(st.isin));
      for (char *p = st.isin; *p; p++) *p = toupper(*p);
      if (strlen(st.isin) != 12 || !isalpha((uint8_t)st.isin[0]) || !isalpha((uint8_t)st.isin[1])) {
        memset(&st, 0, sizeof(st));
        continue;
      }
      // Nom : sans les caractères qui casseraient la page quand elle le réaffiche ; « & » est
      // gardé (S&P 500), et écrit &amp; dans la page
      snprintf(name, sizeof(name), "kn%u", i);
      decode(param(query, name), st.name, sizeof(st.name));
      for (char *p = st.name; *p; p++) {
        if (strchr("'\"<>", *p)) *p = ' ';
      }
      snprintf(name, sizeof(name), "kq%u", i);
      st.quantity = quantity(param(query, name));
      snprintf(name, sizeof(name), "kp%u", i);
      st.cost = quantity(param(query, name));
      count++;
    }
    stocksConfigure(list, count);
  }

  WeatherSettings s;
  weatherSettings(s);
  s.enabled = param(query, "meteo") != nullptr;
  const char *lat = param(query, "lat"), *lon = param(query, "lon");
  if (lat && lon) {
    char *lat_end, *lon_end;
    float la = strtof(lat, &lat_end), lo = strtof(lon, &lon_end);
    if (lat_end != lat && lon_end != lon && fabsf(la) <= 90 && fabsf(lo) <= 180) s.latitude = la, s.longitude = lo;
  }
  weatherConfigure(s);

  BrightnessSettings b;
  brightnessSettings(b);
  const char *day = param(query, "jour"), *night = param(query, "nuit");
  if (day) b.day = constrain(atoi(day), 1, 100);
  if (night) b.night = constrain(atoi(night), 0, 100);
  b.automatic = param(query, "auto") != nullptr;
  b.red = param(query, "rouge") != nullptr;
  b.sleep = param(query, "veille") != nullptr;
  int from = minuteOfDay(param(query, "debut")), to = minuteOfDay(param(query, "fin"));
  if (from >= 0) b.sleep_from = from;
  if (to >= 0) b.sleep_to = to;
  b.deep = param(query, "profonde") != nullptr;

  brightnessConfigure(b);

  astroConfigure({param(query, "soleil") != nullptr, param(query, "lune") != nullptr});
  spaceSetEnabled(param(query, "espace") != nullptr);

  // Capteur de particules : son adresse, réduite à ce qu'une adresse peut contenir
  static AirSettings air;
  airSettings(air);
  air.enabled = param(query, "air") != nullptr;
  air.bar = param(query, "airbar") != nullptr;
  if (param(query, "airhost")) {
    char host[sizeof(air.host)];
    decode(param(query, "airhost"), host, sizeof(host));
    const char *start = strncmp(host, "http://", 7) == 0 ? host + 7 : host;
    size_t n = 0;
    while (start[n] && (isalnum((uint8_t)start[n]) || start[n] == '.' || start[n] == '-') && n < sizeof(air.host) - 1) n++;
    memcpy(air.host, start, n);
    air.host[n] = 0;
  }
  airConfigure(air);
}

// Octets de pile qu'une tâche n'a jamais utilisés, 0 si elle n'existe pas
static unsigned stackMargin(const char *task) {
  TaskHandle_t handle = xTaskGetHandle(task);
  return handle ? uxTaskGetStackHighWaterMark(handle) : 0;
}

static void sendPage(WiFiClient &c) {
  page_len = 0;
  add("%s", HEAD);
  WeatherSettings s;
  weatherSettings(s);
  BrightnessSettings b;
  brightnessSettings(b);
  AstroSettings sky;
  astroSettings(sky);
  static Crypto crypto;  // hors de la pile, comptée au plus juste ; une seule tâche passe ici
  cryptoGet(crypto);
  static AirSettings air;
  airSettings(air);

  // Diaporama : les pages qu'on peut y mettre, l'ordre en place, puis la durée de chacune. La
  // mire de test, qui prend tout l'écran, est rangée avec la maintenance.
  uint8_t count = pluginCount(), test_page = count;
  char order[128], title[32];
  pluginOrder(order, sizeof(order));
  auto titleOf = [&](uint8_t i) {
    const Plugin *plugin = pluginAt(i);
    strlcpy(title, i == count - 1 ? "accueil" : plugin->label ? plugin->label : plugin->name, sizeof(title));
    title[0] = toupper(title[0]);
  };
  add("%s", SHOW_HEAD);
  for (uint8_t k = 0; k < count; k++) {
    uint8_t i = k == 0 ? count - 1 : k - 1;  // l'accueil d'abord
    if (pluginAt(i)->optional) {
      test_page = i;
      continue;
    }
    titleOf(i);
    add(SHOW_ADD, i == count - 1 ? "accueil" : pluginAt(i)->name, title);
  }
  add(SHOW_ORDER, order);
  for (uint8_t k = 0; k < count; k++) {
    uint8_t i = k == 0 ? count - 1 : k - 1;
    const Plugin *plugin = pluginAt(i);
    if (plugin->optional) continue;
    PluginSlide slide;
    pluginSlide(i, slide);
    titleOf(i);
    add(PAGE_SECONDS, title[0], title + 1, i, slide.seconds);
    if (plugin->exclusive) add(PAGE_EXCLUSIVE, title[0], title + 1, i, slide.exclusive ? " checked" : "");
  }
  add(SHOW_TAIL, pluginFade() ? " checked" : "", s.enabled ? " checked" : "", sky.sun ? " checked" : "",
      sky.moon ? " checked" : "", spaceEnabled() ? " checked" : "", air.enabled ? " checked" : "", s.latitude, s.longitude);

  for (uint8_t c = 0; c < crypto.count; c++) {
    char held[24];
    number(crypto.coins[c].quantity, held, sizeof(held));
    add(COIN_ROW, crypto.coins[c].symbol, c, held, c, crypto.coins[c].id, c, crypto.coins[c].symbol, c);
  }
  add("%s", COINS_NOTE);
  static Stocks &shares = *(Stocks *)heap_caps_malloc(sizeof(Stocks), MALLOC_CAP_SPIRAM);  // une seule tâche passe ici
  stocksGet(shares);
  for (uint8_t i = 0; i < shares.count; i++) {
    const Stock &st = shares.stocks[i];
    char held[24], cost[24];
    number(st.quantity, held, sizeof(held));
    number(st.cost, cost, sizeof(cost));
    // Nom donné par la bourse, en texte grisé, et nom choisi : écrits pour un attribut HTML
    char hint[48], label[48];
    attribute(shares.title[i][0] ? shares.title[i] : st.isin, hint, sizeof(hint));
    attribute(st.name, label, sizeof(label));
    add(STOCK_ROW, i, hint, label, i, held, i, cost, i, st.isin, i);
  }
  add("%s", STOCKS_NOTE);
  for (uint8_t u = 0; u < AGENDA_URLS; u++) {
    char name[AGENDA_NAME_SIZE];
    agendaName(u, name, sizeof(name));
    add(URL_ROW, u + 1, name, u + 1, agendaHasUrl(u) ? "adresse enregistrée" : "adresse iCal (https://...)", u + 1);
  }
  PluginSlide test = {};
  if (test_page < count) pluginSlide(test_page, test);
  add(SETTINGS, air.host, air.bar ? " checked" : "", b.day, b.day, b.automatic ? " checked" : "", b.night, b.night, b.red ? " checked" : "",
      b.sleep ? " checked" : "", b.sleep_from / 60, b.sleep_from % 60, b.sleep_to / 60, b.sleep_to % 60,
      b.deep ? " checked" : "", test_page, test.enabled ? " checked" : "");

  char weather[40];
  int32_t age = weatherAge();
  if (!s.enabled) strlcpy(weather, "désactivée", sizeof(weather));
  else if (age < 0) strlcpy(weather, "pas encore lue", sizeof(weather));
  else snprintf(weather, sizeof(weather), "lue il y a %ld min", (long)(age / 60));
  char quotes[40];
  int32_t quotes_age = cryptoAge();
  if (crypto.count == 0) strlcpy(quotes, "aucune choisie", sizeof(quotes));
  else if (quotes_age < 0) strlcpy(quotes, "cours pas encore lus", sizeof(quotes));
  else snprintf(quotes, sizeof(quotes), "cours lus il y a %ld min", (long)(quotes_age / 60));
  static char shares_state[60];
  int32_t shares_age = stocksAge();
  if (shares.count == 0) strlcpy(shares_state, "aucune choisie", sizeof(shares_state));
  else if (shares_age < 0) strlcpy(shares_state, "cours pas encore lus", sizeof(shares_state));
  else snprintf(shares_state, sizeof(shares_state), "cours lus il y a %ld min, en %u s", (long)(shares_age / 60), stocksRoundSeconds());
  if (stocksUnknown()) {
    size_t n = strlen(shares_state);
    snprintf(shares_state + n, sizeof(shares_state) - n, ", %u ISIN inconnu%s", stocksUnknown(), stocksUnknown() > 1 ? "s" : "");
  }
  // Wikipédia : le titre du jour, sans ce qui casserait la page
  static char wiki_state[160];  // les textes de l'état sont hors de la pile, qui est comptée ; une seule tâche passe ici
  strlcpy(wiki_state, "pas encore lu", sizeof(wiki_state));
  if (wikiAge() >= 0) {
    static WikiArticle w;  // hors de la pile ; une seule tâche passe ici
    wikiGet(w);
    static char title[64];
    attribute(w.title, title, sizeof(title));
    snprintf(wiki_state, sizeof(wiki_state), "%s, lu il y a %ld min%s", title, (long)(wikiAge() / 60), w.image ? "" : ", sans image");
  }
  static char agenda[160];
  int32_t lghs_age = agendaAge(AGENDA_LGHS), mine_age = agendaAge(AGENDA_MINE);
  int n = lghs_age < 0 ? snprintf(agenda, sizeof(agenda), "LGHS pas encore lu")
                       : snprintf(agenda, sizeof(agenda), "LGHS lu il y a %ld min", (long)(lghs_age / 60));
  if (mine_age >= 0) n += snprintf(agenda + n, sizeof(agenda) - n, ", les miens il y a %ld min", (long)(mine_age / 60));
  for (AgendaList list : {AGENDA_LGHS, AGENDA_MINE}) {
    if (agendaProblem(list)[0] && n < (int)sizeof(agenda)) n += snprintf(agenda + n, sizeof(agenda) - n, " ; %s", agendaProblem(list));
  }
  char dust[40] = "pas de mesure";
  float pm25, pm10;
  if (airGet(pm25, pm10)) snprintf(dust, sizeof(dust), "PM2,5 %.1f, PM10 %.1f", pm25, pm10);
  unsigned long minutes = esp_timer_get_time() / 60000000;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  // Soleil du jour et lune, tels que la carte les calcule
  char sun[80] = "ni lever ni coucher";
  struct tm noon = t;
  noon.tm_hour = 12;
  noon.tm_min = noon.tm_sec = 0;
  time_t rise, set;
  if (sunTimes(mktime(&noon), s.latitude, s.longitude, rise, set) == SUN_RISES) {
    struct tm r, e;
    rise += 30, set += 30;
    localtime_r(&rise, &r);
    localtime_r(&set, &e);
    snprintf(sun, sizeof(sun), "%02d:%02d à %02d:%02d, phase %u/8", r.tm_hour, r.tm_min, e.tm_hour, e.tm_min,
             moonPhase(now));
  }
  const char *boot;
  switch (esp_reset_reason()) {
    case ESP_RST_DEEPSLEEP: boot = "réveil programmé"; break;
    case ESP_RST_POWERON: boot = "mise sous tension ou reset"; break;
    case ESP_RST_SW: boot = "redémarrage demandé"; break;
    case ESP_RST_PANIC: boot = "plantage"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: boot = "chien de garde"; break;
    case ESP_RST_BROWNOUT: boot = "baisse de tension"; break;
    default: boot = "autre"; break;
  }
  add(STATE, pluginCurrentName(), t.tm_hour, t.tm_min, sun, minutes / 60, minutes % 60, boot, (int)WiFi.RSSI(),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned long)lcdBadFrames(),
      (unsigned long)lcdLateChunks(), (unsigned long)lcdMaxCopyUs(), (unsigned)lcdPalettesLow(),
      brightnessCurrent(), gfxNight(), GFX_NIGHT_MAX, weather, quotes, shares_state, wiki_state, agenda, spacePeople(), dust, esp_ota_get_running_partition()->label, stackMargin("web"),
      stackMargin("meteo"), stackMargin("sonos"), stackMargin("crypto"), stackMargin("actions"), stackMargin("wiki"), stackMargin("agenda"));
  add("%s", FOOTER);
  add("%s", SCRIPT);
  // Une page coupée perdrait son script, et plus aucun réglage ne s'enregistrerait
  if (page_len >= PAGE_SIZE - 1) Serial.println("Back office : page trop longue pour son tampon");

  // Jamais gardée en cache par le navigateur : elle doit montrer les réglages du moment
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
           "Content-Length: %u\r\nConnection: close\r\n\r\n",
           (unsigned)page_len);
  c.write((const uint8_t *)page, page_len);
}

// Reçoit un firmware, envoyé tel quel dans le corps de la requête, et l'écrit dans la partition
// d'application qui ne sert pas. `data` : les `got` premiers octets, arrivés avec les en-têtes.
static void receiveFirmware(WiFiClient &c, uint8_t *data, size_t got, size_t total) {
  if (!total || !Update.begin(total)) {
    reply(c, "400 Bad Request", total ? Update.errorString() : "Fichier vide");
    return;
  }
  Serial.printf("Mise à jour : %u octets à recevoir\n", (unsigned)total);
  updating = true;
  delay(300);  // le temps que la boucle d'affichage passe l'écran au noir

  size_t done = 0;
  uint32_t last = millis();
  while (done < total && millis() - last < UPDATE_TIMEOUT_MS) {
    if (got == 0) {
      int n = c.read((uint8_t *)req, BUF_SIZE);
      if (n <= 0) {
        if (!c.connected() && !c.available()) break;
        delay(2);
        continue;
      }
      data = (uint8_t *)req;
      got = n;
    }
    got = min(got, total - done);
    if (Update.write(data, got) != got) break;
    done += got;
    got = 0;
    last = millis();
  }

  // Update.end() vérifie l'image avant de la désigner pour le prochain démarrage
  const char *error = done < total ? (Update.hasError() ? Update.errorString() : "Envoi incomplet") : nullptr;
  if (!error && !Update.end()) error = Update.errorString();
  Serial.printf("Mise à jour : %s\n", error ? error : "reçue, redémarrage");
  if (error) reply(c, "500 Internal Server Error", error);
  else reply(c, "200 OK", "Mise à jour reçue, redémarrage...");
  c.stop();
  // Echec compris : l'écran a été effacé, repartir de zéro est le plus simple
  delay(500);
  ESP.restart();
}

// Capture de l'écran, telle qu'il est affiché : BMP 16 bits RGB565 de haut en bas, envoyé par
// paquets de lignes copiés dans le tampon des requêtes
static void sendScreen(WiFiClient &c) {
  const uint32_t row = LCD_WIDTH * 2, size = 66 + row * LCD_HEIGHT;
  uint8_t head[66] = {'B', 'M'};
  auto le32 = [&](uint8_t at, uint32_t v) { memcpy(head + at, &v, 4); };
  le32(2, size), le32(10, 66);                               // taille, début des pixels
  le32(14, 40), le32(18, LCD_WIDTH), le32(22, -LCD_HEIGHT);  // en-tête : hauteur négative, de haut en bas
  head[26] = 1, head[28] = 16, le32(30, 3), le32(34, row * LCD_HEIGHT);  // 1 plan, 16 bits, BI_BITFIELDS
  le32(54, 0xF800), le32(58, 0x07E0), le32(62, 0x001F);     // masques rouge, vert, bleu
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %lu\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n", (unsigned long)size);
  c.write(head, sizeof(head));
  const uint16_t rows = BUF_SIZE / row;
  for (uint16_t y = 0; y < LCD_HEIGHT && c.connected(); y += rows) {
    uint16_t n = min<uint16_t>(rows, LCD_HEIGHT - y);
    memcpy(req, lcd_fb + y * LCD_WIDTH, n * row);
    c.write((const uint8_t *)req, n * row);
  }
}

static void handle(WiFiClient &c) {
  // La requête jusqu'à la fin de ses en-têtes : sa première ligne, « GET /chemin?paramètres
  // HTTP/1.1 », dit quoi faire
  size_t len = 0;
  uint32_t t0 = millis();
  req[0] = 0;
  while (len < BUF_SIZE - 1 && millis() - t0 < (len ? HTTP_TIMEOUT_MS : IDLE_TIMEOUT_MS) && c.connected() &&
         !strstr(req, "\r\n\r\n")) {
    int got = c.read((uint8_t *)req + len, BUF_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(2);
    req[len] = 0;
  }
  char *end = strstr(req, " HTTP/"), *body = strstr(req, "\r\n\r\n");
  // Les navigateurs ouvrent des connexions d'avance, sans rien y envoyer. On les referme sans
  // répondre : une réponse y resterait en attente, et le navigateur la prendrait pour celle de la
  // requête qu'il finit par y envoyer. Fermée, il en ouvre simplement une autre.
  if (!end || !body) {
    c.stop();
    return;
  }
  *end = 0;

  if (end && strncmp(req, "GET /set", 8) == 0) {
    apply(req + 7);
    // La page envoie les réglages sans se recharger : rien à lui répondre
    c.print("HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  } else if (end && strcmp(req, "GET /") == 0) {
    sendPage(c);
  } else if (end && strcmp(req, "GET /ecran.bmp") == 0) {
    sendScreen(c);
  } else if (end && body && strcmp(req, "POST /update") == 0) {
    const char *length = strcasestr(end + 1, "content-length:");
    body += 4;
    receiveFirmware(c, (uint8_t *)body, req + len - body, length ? strtoul(length + 15, nullptr, 10) : 0);
  } else if (end && strcmp(req, "POST /reboot") == 0) {
    reply(c, "200 OK", "Redémarrage...");
    c.stop();
    Serial.println("Redémarrage demandé depuis le back office");
    delay(300);
    ESP.restart();
  } else {
    c.print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }
  c.stop();
}

static void webTask(void *) {
  WiFiServer server(80);
  bool listening = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      delay(500);
      continue;
    }
    if (!listening) {
      server.begin();
      listening = true;
      Serial.printf("Réglages : http://%s/\n", WiFi.localIP().toString().c_str());
    }
    WiFiClient c = server.accept();
    if (c) handle(c);
    else delay(50);
  }
}

void webBegin() {
  req = (char *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
  page = (char *)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_SPIRAM);
  if (req && page) xTaskCreatePinnedToCore(webTask, "web", 4608, nullptr, 1, nullptr, 0);
}

bool webUpdating() {
  return updating;
}
