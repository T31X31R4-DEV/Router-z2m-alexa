# Z2M → Alexa (ESP8266)

Faz a **Alexa enxergar os dispositivos do Zigbee2MQTT**. Um ESP8266 na rede das Echo se apresenta à Alexa como uma **ponte Philips Hue** e conversa com o Zigbee2MQTT **só pelo MQTT**:

```
Alexa ──Hue (rede local)──▶ ESP ──MQTT──▶ zigbee2mqtt/<nome>/set ──▶ Z2M ──▶ dispositivo Zigbee
Alexa ◀──── estado ──────── ESP ◀──MQTT── zigbee2mqtt/<nome>     ◀── Z2M
```

O ESP **não se conecta ao coordenador** (SLZB-06M). Por isso convive com o Zigbee2MQTT do Home Assistant sem interferir. Ligar dois programas no mesmo coordenador quebraria a rede Zigbee.

Não depende do Home Assistant: só do broker MQTT e do Zigbee2MQTT. Substitui o Tasmota com relés "fantasma" e o blueprint de espelho.

* Até **20 dispositivos**, cadastrados pela **página web do ESP**, sem recompilar.
* Tipos: **tomada** e **luz** (liga/desliga) ou **dimmer** (com brilho: "Alexa, sala a 40%"). Na Alexa, **luz** e **dimmer** aparecem como lâmpada; **tomada** aparece como tomada (veja "Tomadas na Alexa").
* Interruptores de várias teclas: informe a propriedade (`state_l1`, `state_l2`…).
* Estado em mão dupla: se ligarem pelo botão, pelo HA ou pelo Z2M, a Alexa mostra certo.

## 1. Gravar o firmware

Arquivos: `firmware/z2m-alexa-v1.1.0-esp8266-4MB.bin` e a versão compactada `.bin.gz` (D1 mini, NodeMCU e outros ESP8266 com **4 MB** de flash).

> **Confirme antes que a flash é de 4 MB.** No Tasmota: *Informações → Flash Size*. O `.bin` comum é recusado se a flash for menor, mas o **`.bin.gz` não passa por essa checagem**: num ESP de 1 MB ele seria gravado e o aparelho não iniciaria, e aí só o cabo resolve.

**Se o ESP já tem Tasmota:** o Tasmota de ESP8266 só tem uns 300 KB livres para receber atualização, então o `.bin` comum (400 KB) é recusado com "sem espaço". Use um destes caminhos, ambos sem cabo:

1. **Direto, com o `.bin.gz` (284 KB):** veja em *Informações → Free Program Space* se há pelo menos 289 KB. Se houver, vá em *Atualização de firmware → Atualizar pelo arquivo* e envie o `.bin.gz`.
2. **Em duas etapas (sempre funciona):** primeiro envie o **`tasmota-minimal.bin.gz`** oficial, baixado do site do Tasmota. Ele é pequeno, mantém o Wi-Fi e volta no mesmo IP, só com a página de atualização. Em seguida, na página dele, envie o `.bin.gz` (ou o `.bin`) deste firmware. É o mesmo caminho que o Tasmota usa para as atualizações grandes.

Em ambos os casos, o Tasmota só é substituído quando o arquivo chega inteiro. Se falhar, ele continua como estava. Depois da troca, este firmware abre a rede `z2m-alexa-XXXX` para a configuração (seção 2).

**Se o ESP já está com este firmware:** use `http://IP-DO-ESP/update` (formulário *Firmware*). Aqui o espaço livre é maior, e o `.bin` comum entra direto.

**Pelo USB:** use um gravador web (por exemplo, o *ESP Tool* da Espressif ou o *ESPHome Web*, pelo Chrome ou Edge). Grave o `.bin` no endereço `0x0`. Ou pela linha de comando:

```bash
esptool.py --port /dev/ttyUSB0 write_flash 0x0 z2m-alexa-v1.1.0-esp8266-4MB.bin
```

## 2. Configurar Wi-Fi e MQTT

Na primeira vez, o ESP abre uma rede Wi-Fi **`z2m-alexa-XXXX`** (senha **`configurar`**). Conecte o celular nela e abra **http://192.168.4.1/**. Preencha:

| Campo | Valor |
|---|---|
| Wi-Fi | a rede das Echo (o TP-Link) |
| Broker MQTT | o endereço pelo qual o ESP alcança o Mosquitto. **Na sua instalação com VPN:** o IP do túnel do `openvpn_client` com o `socat` (`mqtt_reverso`), o mesmo que o Tasmota usava |
| Usuário / senha MQTT | um usuário do Mosquitto |
| base_topic | `zigbee2mqtt` (ou o que estiver no `configuration.yaml` do Z2M) |
| Senha da página | obrigatória; protege a página e a atualização de firmware |

Ao salvar, o ESP reinicia e entra no seu Wi-Fi. Se não conseguir conectar em 60 s, a rede de configuração volta a abrir sozinha.

## 3. Cadastrar dispositivos

Acesse **http://IP-DO-ESP/** (login com a senha da página) e, em *Adicionar dispositivo*, preencha:

* **Nome na Alexa:** o que você vai falar ("Luz da Sala").
* **friendly_name no Z2M:** exatamente como aparece no Zigbee2MQTT (diferencia maiúsculas; pode ter espaços e `/`).
* **Tipo:** tomada, luz ou dimmer.
* **Propriedade:** `state` na maioria; `state_l1`, `state_l2`… em interruptores de várias teclas (veja na aba *Exposes* do Z2M).

O ESP reinicia (leva uns 5 s). Em seguida diga **"Alexa, descobrir dispositivos"**. A página mostra o estado de cada um e tem um botão para testar.

**Remover** um dispositivo muda o número interno dos que vêm depois. Rode a descoberta de novo e apague no app da Alexa os que ficarem "sem resposta".

## Tomadas na Alexa (1.1.0)

Em **Wi-Fi e MQTT → Alexa**, escolha como os dispositivos do tipo **tomada** se apresentam:

| Opção | Como funciona |
|---|---|
| **Tomada (perfil Hue Smart plug)** — padrão | Pela ponte Hue, como a tomada da Philips (`On/Off plug-in unit`, modelo `LOM001`). |
| **Tomada (emulação WeMo/Belkin)** | Cada tomada vira um "Belkin WeMo Socket" (portas 49153, 49154…), o mesmo que o Tasmota faz com `Emulation 1`. A Alexa sempre mostrou o WeMo como tomada. |
| **Lâmpada** | Como até a 1.0.x. |

**Ao trocar a opção:** no app Alexa, **exclua** as tomadas antigas (a Alexa guarda o tipo antigo) e diga **"Alexa, descobrir dispositivos"**. Se continuarem aparecendo como lâmpada, teste a outra opção de tomada.

**Sem garantia da Amazon:** não há documentação de como a Echo classifica um aparelho achado localmente, e a Belkin encerrou o serviço WeMo em 31/01/2026 (a descoberta local pela Echo não depende dele, mas a Amazon pode mudar isso). Por isso as duas opções existem: use a que a sua Echo mostrar como tomada.

A página mostra, em cada dispositivo, como ele está sendo anunciado ("Alexa: tomada Hue", "tomada WeMo :49153"…).

## Novidades

* **1.1.0:** tomadas aparecem como **tomada** na Alexa (perfil Hue Smart plug ou emulação WeMo, à escolha); a página mostra como cada dispositivo é anunciado. A biblioteca Espalexa passou a vir dentro do projeto, com esses ajustes.

* **1.0.2:** a lista se atualiza sozinha a cada 3 s (estado, offline, MQTT) e o botão liga/desliga funciona sem recarregar a página; depois de adicionar ou remover, a página volta para a lista assim que o ESP termina de reiniciar.
* **1.0.1:** adicionar/remover não leva mais a reenviar o formulário; mensagem clara de nome repetido; `.bin.gz` para atualizar a partir do Tasmota.

## Limites

* A Alexa precisa de um **Echo físico na mesma rede** que o ESP. O app sozinho não descobre pontes Hue.
* O jeito como a Alexa classifica uma tomada emulada depende da Amazon (veja "Tomadas na Alexa"); ligar e desligar funciona em qualquer caso.
* Ativar **IGMP** no roteador ajuda na descoberta (a busca da Alexa é multicast).
* Se o Wi-Fi cair, o ESP reinicia ao voltar, para refazer a descoberta da Alexa.
* Sem MQTT, a Alexa ainda acha os aparelhos, mas os comandos não chegam. A página mostra "MQTT desconectado".

## Diagnóstico

* `http://IP-DO-ESP/estado.json`: MQTT, Alexa, memória e estado de cada dispositivo.
* "sem resposta do Z2M": o `friendly_name` ou a propriedade não batem, ou o dispositivo ainda não publicou estado. Confira no Z2M em *MQTT explorer* / aba *State*.
* Serial a 115200: mostra cada comando da Alexa (`Alexa: Luz da Sala → ON`) e cada envio (`→ zigbee2mqtt/…/set`).
* Status do ESP no MQTT: `z2m-alexa/status` = `online`/`offline` (retido).

## Compilar

**Arduino IDE / arduino-cli:** núcleo ESP8266 3.1.2; bibliotecas PubSubClient 2.8, ArduinoJson 7.4.2 e Espalexa (GitHub `Aircoookie/Espalexa`); placa *LOLIN(WEMOS) D1 mini*, flash *4MB (FS:2MB)*. Abra `z2m_alexa/z2m_alexa.ino`.

**PlatformIO:** `pio run -t upload` (o `platformio.ini` já fixa as versões).

O `config.h` é opcional: serve para compilar já com Wi-Fi e MQTT (copie de `config.h.example`).
