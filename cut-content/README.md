# inFAMOUS Second Son: cut content

**English** · [Русский](README.ru.md)

> [!WARNING]
> **Spoilers.** This page discusses the whole story of inFAMOUS Second Son, the ending choices included, and the voice lines and videos here give away plot points. If you have not finished the game, finish it first.

Everything below was found in the game's own files: `eboot.bin`, the packages in `art\cache\*.xpps`, the voice archives in `art\cache\psarc\*.psarc` and the videos in `art\movies`. The build examined is **"retail 0212.1422 i3_eu ship"**, the European retail release (CUSA00004). The game's internal name is **i3**, inFAMOUS 3.

"Unused" here means that nothing refers to the file or text: neither the game's code nor any mission's data. References were searched for both by name and by the hash of the name (h·101). That is strong evidence of cut content, but not absolute proof: in rare cases the game builds a name at run time.

The voice lines come from the game in two languages: `_en.mp3` is the original, `_ru.mp3` the official Russian dub.

**Contents:** [Story](#story) · [Missions and objectives](#missions-and-objectives) · [District activities](#district-activities) · [Powers](#powers) · [Systems and difficulty](#systems-and-difficulty) · [The D.U.P. dossier](#the-dup-dossier) · [Trophies](#trophies) · [Videos](#videos) · [Beta, demo, debug](#beta-demo-debug) · [How it was found](#how-it-was-found)

---

## Story

### The Akuran gang was Russian

Inside the game the gang is still called `russians` and `siberians`:
- a character package of its own, `russians.xpps`;
- animations `RussianTaunt`, `RussianCall`, `RussianMelee`;
- the messages "Akura Eliminated" and "AKURAN GANG MEMBER" are `u_gain_russians` and `u_legend_angry_russians`;
- Eugene's mission against the gang uses `vde_ambush_siberians`, `vde_eliminate_siberians` and lines named `deadsiberian`.

Lines that still call them Russian:

| Who | Line |
|---|---|
| Delsin | *"Better find that poor SBT on the crane before those blood-thirsty **Russians** do."* |
| Delsin | *"So Eugene, I just smashed up a bunch of those obnoxious **Russian** Akuran guys…"* |
| Delsin | *"…ripping the intestines out of these **eastern bloc** loud mouths…"* (the Russian dub softened it to "intolerant thugs") |
| Gang member | *"Ha ha, they do not understand English!"* |
| Gang member | *"Yeah, maybe you could teach them English!"* |
| Gang member | *"Hey painter, how about if I paint your face with my fist?!"* |
| Gang member | *"Look out! The painter has a sword!"* |

🔊 [`audio/russians_en.mp3`](audio/russians_en.mp3) · [`audio/russians_ru.mp3`](audio/russians_ru.mp3)

### Blast Cores instead of Core Relays

What Delsin drains for his concrete powers is a *Core Relay* in the final game. It used to be a **Blast Core**, as in the earlier games. Traces are left in several texts: "The DUP are sending a large force to reclaim their **blast core**", "Gotta drain that Blast Core", "I need a blast core!".

A cut scene from the **first mission** survives whole: Delsin touches a core and gets a new power.

> **Reggie:** Whoa whoa whoa whoa, what are you doing?! Delsin, Jesus, you okay?!
> **Delsin:** You remember when I touched that sweaty bald guy and got those super-powers? …Well, I think I just got a new one…
> **Reggie:** We're trying to make what happened to you go away, not get worse.
> **Delsin:** You think Augustine's getting rid of HER powers, huh?
> **Delsin** (later): Okay, not quite the jolt of the Blast Cores back there, but still pleasant. In an awesome way.

🔊 [`audio/blast_cores_en.mp3`](audio/blast_cores_en.mp3) · [`audio/blast_cores_ru.mp3`](audio/blast_cores_ru.mp3)

### The ending: a third choice and "the band back together"

- **The ending once depended on Hank, not on karma.** Two unused texts: *"Mission Kill Augustine is not available because you chose to spare Hank"* and *"Mission Expose Augustine is not available because you chose to kill Hank."* The choice "Kill Hank / Spare Hank" is in the game, but in the final version the karma level decides which ending you get.
- A line about a **third choice** (`m_cb_thirdchoice`): *"Oh, no no, this is a big decision. Come on, I gotta get my head on straight."*
- It looks like **Eugene and Fetch helped** in the final fight: *"Hey, I heard you were getting the band back together."*, *"I'm with you big guy, me and all my friends."*, *"Hey Smokes, need some help?"*, *"You're lucky I was available."*
- Unused lines of the evil ending: *"Sic semper tyrannus…bitch."*, *"Guess it's time to go home."*, *"Now…I'll stop."* And Augustine: *"Those people deserved a REAL hero."*
- Fetch, as Delsin frees her: *"First you lock me up, now you're breaking me out. What's up with that, D?"* Augustine in the fight: *"Join your brother!"*
- A placeholder from the Augustine fight: *"Delsin uses an ionic power to attack Augustine, which causes her to flee temporarily."*

🔊 [`audio/finale_en.mp3`](audio/finale_en.mp3) · [`audio/finale_ru.mp3`](audio/finale_ru.mp3)

### Hank and his daughter

Voiced lines for **Hank's daughter**, who is not in the game: *"Dad? Daddy? Let's go."*, *"Dad, where are you? This place scares me, I want to go."* And unused lines from the hunt for Hank: *"Dammit! Never got an answer about Hank… At least I know he's out there…"*

🔊 [`audio/hank_en.mp3`](audio/hank_en.mp3) · [`audio/hank_ru.mp3`](audio/hank_ru.mp3)

### Reggie

- **"Lay low."** Reggie wanted Delsin to keep his head down and offered to ask his cop friends in Seattle. Delsin: *"Uh, I did not come here to 'lay low'."*, *"Fine, well then let's gouge out their eyes and slice off their ears."*
- **Agents in green caps** with a bear paw on them (*"So, find the meanies in green beanies and take 'em out?"*). Delsin finds the hidden cameras with a phone app from Betty: *"Dude, promise when we get home you will not arrest Betty for stealing cable."*
- **"I'm going alone."** An argument in which Delsin refuses to take Reggie along: *"Reg, I don't need you to 'let me' do anything. You can't 'make me' or 'stop me'… And I've decided, you are NOT going."* With it goes the hint *"Press … to turn Invisible and leave Reggie behind."*

🔊 [`audio/reggie_lay_low_en.mp3`](audio/reggie_lay_low_en.mp3) · [`audio/reggie_go_alone_en.mp3`](audio/reggie_go_alone_en.mp3) (and `_ru`)

### Fetch

- The fight with her was to be set in Seattle's real **Crocodile** club: *"Inside the Crocodile. Laser Girl's with me."*
- An older way to the Lantern District over a **broken bridge**, with a "conference call": Reggie and Fetch are both on the line. Reggie: *"Oh good…this is a conference call."*, *"You know what, two's company, three's a-nnoying. I'm gone."* Fetch: *"Man, how do you put up with that?"*

🔊 [`audio/crocodile_en.mp3`](audio/crocodile_en.mp3) · [`audio/old_bridge_call_en.mp3`](audio/old_bridge_call_en.mp3) (and `_ru`)

### The Space Needle and Delsin

- Another version of the Space Needle climb, looking for **elevators**: *"Reggie, DUPs have the elevator locked down. You got any cop tricks to override it?"* / *"Hot-wiring elevators? Not exactly something they teach you at cop school."* Delsin: *"Do you have any idea how many people are injured in elevator accidents every year? I'm climbing to the top."*
- Delsin tries a nickname, *"Smokestack Jack"*, and dreams: *"If I had smoke AND lasers, I could get a job doing the pre-game introduction of NBA players."*

🔊 [`audio/space_needle_en.mp3`](audio/space_needle_en.mp3) · [`audio/space_needle_ru.mp3`](audio/space_needle_ru.mp3)

### Small things

- **The working girls of Lower Granite** thank Delsin for clearing the streets of dealers: *"Hey! The hero man to working girls everywhere!"* 🔊 [`audio/working_girls_en.mp3`](audio/working_girls_en.mp3)
- **The reservation beach:** Delsin talks to his old graffiti (*"I had almost forgotten about you. My little turtle buddy."*) and races to the Longhouse. 🔊 [`audio/beach_graffiti_en.mp3`](audio/beach_graffiti_en.mp3)

---

## Missions and objectives

- **"Surveillance Van Mission".** There is a title, an objective "Destroy Undercover Van", a per-district count "Surveillance Van Found" and a "SURVEILLANCE VAN" map marker. The code keeps an object type `UndercoverVan`, and the data a package `undercover_van.xpps` with a model and physics. No mission uses it.
- A hint *"A D.U.P. Informant has hidden a hard drive in the area. Use your phone to locate it."*: a cut kind of hidden item.
- A hint *"Press … from behind for a stealthy takedown."*: stealth takedowns.
- A **playtest** objective, `u_banner_playtest_03_chase`: *"Catch Fetch and drain her powers."*
- An older **Space Needle** mission: *"Go to the base of the Space Needle"*, *"Find the elevators at the base of the Space Needle"*, *"Go to the rooftop"*, and the fail message *"You fell off the Space Needle"*.
- Unused objectives: *"Check on Reggie before exploring the Conduit's lair"*, *"Help the escaped conduit"*, *"Open the back of the dealer truck"*, *"Get aboard the bus"*.
- Delsin asks Eugene to help in the streets (`m_help2`): *"Hey Eugene, I know you're probably busy wrestling with your demons, but those guys in the D.U.P. vests really need a little…"*
- Loading-screen trivia: *"Henry 'Hank' Daughtry has six toes on his right foot"*; *"The series 'D.U.P. Miami' is currently the third highest rated show on television."*

## District activities

| What | Traces |
|---|---|
| **Surveillance vans** | see above |
| **Watchtowers** | "Watch Tower Destroyed" |
| **Evacuation** | Police hold back a crowd at the helicopters, people wave money: *"The helicopter is already full!"*, *"I cannot take your money, the helicopters are full!"*, *"There will be more helicopters soon!"* The code keeps the crowd actions `HoldBackMob` and `WaveMoney`. 🔊 [`audio/evacuation_en.mp3`](audio/evacuation_en.mp3) |
| **Signal jammers** | The D.U.P. jams the area and Delsin loses his map: *"Map's out, better find the jammer."*, *"I better cut the juice to the jammer, get my GPS back."* None of these lines is ever played. 🔊 [`audio/signal_jammer_en.mp3`](audio/signal_jammer_en.mp3) |
| **Street musicians** | An evil-karma event, "Street Musician Silenced", with 20 voiced reactions from the performers: *"I just want to make people happy!"*, *"You can't suppress beauty, dude!"*, *"I'm a single mom, please!"* Neither the event nor the lines are used; the map icon survives (see below). 🔊 [`audio/street_musicians_en.mp3`](audio/street_musicians_en.mp3) |
| **Helicopters in district fights** | *"Are you kidding me, helicopters?! What, outnumbering me a hundred to one isn't good enough for 'em?"*, *"I gotta bring those choppers down."* 🔊 [`audio/helicopters_en.mp3`](audio/helicopters_en.mp3) |
| **Replaying districts at a higher difficulty** | *"Congratulations! You have destroyed all the D.U.P. Mobile Command Centers. The D.U.P. Command Centers can be reset to a HARDER state from the Options screen."* An option "Reset D.U.P. Mobile Command Centers" and a warning *"This will make the city much more dangerous!"* |

## Powers

The game's list of powers (`POWERK_*`) keeps powers that are not in the upgrade menu:
- **HealOther**: heal others;
- **PassiveShield** and **UpgradePassiveShieldOvercharge**: a passive shield;
- **DrainObject**: drain objects;
- **LaserBeam** (with cost upgrades BeamCost1/2): a continuous neon beam;
- **ConcretePull**;
- **TvSummonAngel**: summon an angel;
- **TvMeleeGood/Evil**: good and evil video melee;
- **SpyCamera**;
- **TwoPowerSets / ThreePowerSets**;
- **Smoke/Laser/TvUpgradeIonic**, **SmokeUpgradeOrbitalDrop**, **LaserUpgradeDashDouble**;
- **ProtoUpgrade15–20** and empty slots.

The text table describes upgrades that are not even in the code: concrete energy capacity +80 % and ×2, cheaper shots (−25 % / −50 %), faster draining, 3 to 5 Concrete Barrage missiles, up to 50 % less damage taken, and +25 % / +50 % damage to armored enemies for smoke, neon and video.

**Alternative power names:** Concrete Tornado, Wing Surge, Heaven's Hellfire, Video Jump, Summon Guardian, Channel Surfing. Paper is called **steel** inside the game.

**Old controls:** *"Press and hold the Touch Pad Button with both thumbs to drain…"*

## Systems and difficulty

- **Challenge Mode.** The settings keep unused difficulty values: **Story Mode**, **Challenge Mode** and an old **Hard**. The final game offers Easy / Normal / Expert.
- **An XP system.** A whole set of "+N **XP**" messages: "Enemy Obliterated", "Civilian Killed", "Rampage", "Surveillance Van Destroyed", "Suspects Killed", with a multiplier "(x)". In the final game upgrades are bought with Blast Shards.
- **A "prototype" time of day.** The city lists a debug time of day, `time_of_day_prototype_001`, with its own sky `skybox_prototype_001` and lighting packages `*-world_seaport(prototype)`. The real weathers have transition videos; this one has none.

## The D.U.P. dossier

The pause menu has a dossier on Delsin. The game does not use part of it:
- **Aliases:** **"Phoenix Jones"** (a real-life street superhero from Seattle), "Mr. Cool", "Laser Guy", "TV Star".
- **Charges:** "Espionage", "Illegal Wiretapping", "War Crimes", "Entering Restricted Airspace", "Disrupting D.U.P. Operations".
- **Sections:** "Case Photos", "Public Image", "Recent Activity", "Last Seen Wearing".
- Every karma level has a police-dossier description, for example: *"An unsuccessful artist with a history of trespassing and vandalism…"*. The starting level, "Vandal", still has the placeholder **"Vandal Desc"**.

## Trophies

The text table keeps two trophies that are not in the final game:
- **Deadliest Catch**: *"Subdue a D.U.P. Commander without killing him"*;
- **Hard Mode**: *"Finish the game in Hard Mode"*.

"Unstoppable" has three descriptions: the final *"Finish the game on Expert difficulty"*, an older *"Finish the game on Hard difficulty"* and **"Finish the game in Challenge Mode"**.

## Images

The UI packages store their textures as raw PS4 GPU data. We decoded all 3,845 of them (format, tiling and names are documented in the research notes).

### Tutorial cards for a district system that isn't in the game

The first district mission (`msn_ftp`) carries two full-screen tutorial cards. The final game never shows them, and they describe things it doesn't have: a district state called **"D.U.P. MONITORED"** and **undercover vans** shown on the map. Their text is baked into the image in English, while every real tutorial in the game is localized text. The map in the screenshot is an early version, with a different district border and icons. And "pissed off Augustine" sounds like it was never meant to ship.

| | |
|---|---|
| ![D.U.P. Mobile Command](images/tutorial_dup_mobile_command.jpg) | ![D.U.P. Monitored](images/tutorial_dup_monitored.jpg) |
| *"Destroying it lessens their overall influence: fewer troops, fewer APC patrols, happier citizens, pissed off Augustine."* | *"A district in this state has a lessened D.U.P. influence, but security cameras, scanning stations and undercover vans still blanket the area."* |

### Map icons

The map icon set, as it ships. Some of the icons belong to activities listed above that were cut: **musician** (the street musicians), **radar_jammer** (the signal jammers), **helicopter**. We can't prove from the data alone which icons the final map never shows, so treat the rest as leads.

![Map icons](images/map_icons.png)

### Placeholder art

Among the map icons sits a 64×64 **photo of a toy school bus**, next to a plain orange `icon_bus`. Placeholder art that was never replaced:

![Toy bus](images/placeholder_toy_bus.png)

### Developer glyphs in the button font

The font that draws the controller buttons (it ships in every language) also holds **PC mouse cursors** (arrow, hand, I-beam, move and resize), a walking man, a lightning bolt and a **"T"** key. They belong to the developers' in-game tools, not to the game:

![Font atlas](images/font_atlas_dev_glyphs.png)

**Not found:** icons for the cut powers (HealOther, LaserBeam, TvSummonAngel…). In the packages we decoded, the upgrade screen has only frames and borders, with no power icons at all, final or cut.

## Videos

The game's videos (`.bsf`) are plain H.264 streams. Of its 159 videos, five are used nowhere:

| File | What it is |
|---|---|
| [`video/v_ustv_cb02.mp4`](video/v_ustv_cb02.mp4) | **A cut USTV news broadcast.** "BREAKING NEWS: SALMON BAY, WASHINGTON": aerial pictures of a destroyed settlement, then a photo of Delsin. The ticker is placeholder text, "Lorem ipsum dolor sit amet…". The game has no audio for it. |
| [`video/splash_europe.mp4`](video/splash_europe.mp4) | The European splash, "Sony Computer Entertainment presents" and the Sucker Punch logo, 1080p. Possibly chosen by region. |
| [`video/tv_heaven_hell_2x2.mp4`](video/tv_heaven_hell_2x2.mp4), [`video/v_ustv_heaven_hell_2x2.mp4`](video/v_ustv_heaven_hell_2x2.mp4) | A heaven-and-hell swirl for screens. |
| [`video/v_tv_snow.mp4`](video/v_tv_snow.mp4) | TV static. |

<img src="images/ustv_cb02_salmon_bay.jpg" width="49%"> <img src="images/ustv_cb02_delsin.jpg" width="49%">

**The anchor's voice, `v_ustv_cb01`: the tribe was killed, and Delsin was blamed.** Only the voice of this broadcast is left, in English and Russian (51 s), and it tells a story the final game does not:

> *"We're showing you now the first images coming into our studio from the small Native American reservation town of Salmon Bay, Washington, the scene of a gruesome attack earlier today. Initial reports are that the **entire Akomish tribe was wiped out** during this violent rampage, which apparently erupted without warning and tragically left no survivors. The D.U.P.'s rapid response team reports that all evidence points to the massacre being the handiwork of a Bio-terrorist using smoke powers. Specifically, they named 23-year-old **Delsin Rowe**, a former member of the Akomish tribe, and according to D.U.P. head Brooke Augustine, the mastermind behind the reign of terror currently gripping Seattle."*

In the final game Augustine wounds the tribe; here the whole tribe dies and Augustine frames Delsin for it. The cb02 video (aerial pictures of a destroyed Salmon Bay, then Delsin's photo) shows the same story, but the two are not one file: in every broadcast the game uses, audio and video match in length to within a tenth of a second, and here it is 51 s against 44 s, so they are likely two versions of the report. *(The quotes from voice lines without subtitles in this section are automatic transcriptions (Whisper); the names are corrected to their known spelling. Listen to the files for the exact words.)*
- 🔊 [`audio/news_cb01_en.mp3`](audio/news_cb01_en.mp3) · [`audio/news_cb01_ru.mp3`](audio/news_cb01_ru.mp3)
- 🎞 [`video/ustv_cb02_plus_cb01_voice_en.mp4`](video/ustv_cb02_plus_cb01_voice_en.mp4) (and `_ru`): **an edit made for convenience**, the cb02 picture with the cb01 voice.

### Trailer and demo soundtracks

The videos of the E3 and Gamescom trailers are gone, but their **audio tracks are still in the game's voice archives**, made for the show-floor and demo builds:

| File | What it is |
|---|---|
| 🔊 [`audio/trailer_e3_2013_en.mp3`](audio/trailer_e3_2013_en.mp3) | **The E3 2013 demo, 4 min 10 s.** Reggie: *"Do you have any idea how embarrassing it is for me to keep arresting my own brother over and over again?"* An earlier plan: *"Alright, I got this whole thing figured out. We hit the town… and then we're back home to save some lives."* |
| 🔊 [`audio/trailer_gamescom_en.mp3`](audio/trailer_gamescom_en.mp3) | **The Gamescom 2013 trailer**, Fetch's: *"My name is Abigail Walker, but my friends and enemies call me Fetch."* |
| 🔊 [`audio/trailer_ps4_launch_en.mp3`](audio/trailer_ps4_launch_en.mp3) | **The PS4 launch trailer**: *"This fast healing stuff is gonna come in real handy."* |
| 🔊 [`audio/demo_preview_intro_en.mp3`](audio/demo_preview_intro_en.mp3) · [`_ru`](audio/demo_preview_intro_ru.mp3) | **The demo's intro, 2 min 5 s, dubbed in Russian too.** A condensed early version of the opening: Reggie arrests Delsin, Hank's escape, *"I'm one of them, man"*, and a different talk about a cure: *"Shooting smoke out of your fingertips isn't exactly normal, man." — "Just because it's not normal doesn't mean that it needs a cure."* |

### The D.U.P.'s loudspeakers

**Propaganda.** Sixteen recorded loudspeaker announcements, "a quick reminder from your friends at the D.U.P.", that the game never plays:
> *"It is better to falsely accuse 50 innocent people if it means the capture of one Bio-terrorist than it is to accuse no one."*
> *"Report your suspicions and be a hero, or remain silent and be a corpse. Please choose wisely."*
> *"They want to kill your friends, your neighbors, and your family pets."*
> *"If you dissent, the Bio-terrorists win."*
> *"We live in the country that wiped out polio, leprosy, and malaria. Together, we can and must wipe out the Bio-terrorist cancer… Be part of the cure."*
> *"It's not snitching if you're saving lives."*
> *"As you approach this checkpoint, please remove any gloves, rings, hats, shoes, socks, watches, and bodily piercings… under provisions of the 2011 Vancouver Security Pact…"*

🔊 [`audio/dup_propaganda_en.mp3`](audio/dup_propaganda_en.mp3) · [`audio/dup_propaganda_ru.mp3`](audio/dup_propaganda_ru.mp3), 5 min each

**District announcements**, for when Delsin attacks the D.U.P.: *"The assembly of more than five citizens is considered a potential threat to public safety"*, *"To date, we have detained over 100 of your fellow citizens"*, *"Now deploying retaliatory shock troops to affected area"*, *"Command post failure detected."* 🔊 [`audio/dup_announcements_en.mp3`](audio/dup_announcements_en.mp3) · [`_ru`](audio/dup_announcements_ru.mp3)

**Gene scanners with threat levels:** *"Please place your hand on the scanner… Processing… Warning: positive scan, level four. Possible genetic carrier. Detain subject for further testing"*, then level three, level two, and *"WARNING, ALL PERSONNEL. LEVEL ONE, PRIME CONDUIT DETECTED. LETHAL FORCE IS AUTHORIZED."* 🔊 [`audio/dup_scanners_en.mp3`](audio/dup_scanners_en.mp3) · [`_ru`](audio/dup_scanners_ru.mp3)

**News and radio:** reporter **Kurt Wyatt** *"from the scene of a massive battle between the D.U.P. and a small group of Bio-terrorists"*; a car radio in the first mission on *"the spectacular decommissioning of the five-twenty bridge"*, blown up so the Bio-terrorists could not leave the city; the loudspeaker countdown *"decommission sequence to commence in three, two, one"*. 🔊 [`audio/news_and_radio_en.mp3`](audio/news_and_radio_en.mp3) · [`_ru`](audio/news_and_radio_ru.mp3)

Contact sheets of all the videos are in [`images/`](images/).

## Beta, demo, debug

- **Build modes.** The code lists the modes the game could be built in: *Normal, Focus, Demo, Floor, Sampler, Preview, Kiosk, Taste, Beta, Retail, Review, Moderator*. So there were show-floor builds (Floor, Kiosk), demos (Demo, Sampler, Taste), a beta and press builds (Review).
- **Trailers for show builds:** `v_e3_trailer`, `v_e3_countdown`, `v_e3_logo_loop`, `v_gamescom_trailer`, `v_gamescom_countdown`, `v_ps4_launch_trailer`, `v_demo_preview_intro`, `v_zeke_date`. The videos are not in the game, but the audio of most of them is: see [Trailer and demo soundtracks](#trailer-and-demo-soundtracks).
- **A debug load menu** (`load_menu_catalog.log`): test worlds `combat_test_world_01`, `x_parkour_test`, `dup_control_room`, `djm_xn_world`, `testscene2`, and sets `i3_experiments`, `char_viz`, `ctw1`, `djm_xn_story`, `rc`. Their packages are not in the retail build. The code keeps `DebugMenu`, `DevMenu` and a `TeleportToMark`.
- **"batman"** is the internal name of the videos of Augustine in the control room (`v_ctn_batman_controlroom`, `v_batman`).
- **A PC version was never planned.** The code has no DirectX, XInput, Steam or Windows. Its key table (Escape…Num /, mouse) belongs to the developers' tools. The PS4 mouse library is linked, but the game never calls it.

## How it was found

- The texts come from the tables `lang_english_text.xpps` and `lang_russian_text.xpps`: 12,133 strings, each checked for references in every game package and in `eboot.bin`.
- The voice lines were extracted from the `psarc` archives and decoded with [vgmstream](https://github.com/vgmstream/vgmstream).
- The videos were rewrapped from raw H.264 to mp4 without re-encoding (`ffmpeg -f h264 -i x.bsf -c copy x.mp4`). The edit and the contact sheets were made separately.
- None of this is turned on in the game by SecondSonPC: these are findings only. What is turned on, separately: the Paper Trail online missions, the add-ons and trophies, see the main [README](../README.md#online-missions-add-ons-and-trophies).

All rights to the game's material belong to Sony Interactive Entertainment and Sucker Punch Productions. It is shown here as research into the game's development history.
