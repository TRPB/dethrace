# Dethrace Slop Pack

Modded [Dethrace](https://github.com/dethrace-labs/dethrace) with several tweaks to make the game more to my liking. No pull requests to upstream because this is heavily AI assisted and just for fun. A wishlist for what I want in the real project with a dirty AI implementation.

Consider this more like a mod than an attempt to improve Dethrace itself, let's leave that project pure.

## Features
- Custom resolution support (3dfx mode only)
- Widescreen support (3dfx mode only)
- Widescreen 2d cockpit support if the 21:9 2d cockpits are present (e.g. MELD pack) they are used for any widescreen resolution
- MSAA support (3dfx mode only)
- Sample rate shading (supersampling) support (3dfx mode only)
- Opponent car level of detail always max + view distance increased 10x
- Pedestrian/powerup/sprite view distance increased 10x 
- Allow configratation of stealworthyness (all cars can be stolen, steal percentage probability, and disabling of rank gate)
- Configureable tyre skid decal limits (blood trails, tyre marks and oil last significantly longer, normally the whole race)

## Bug fixes
- Camera judder fix when car is driving up ramp (rendering only, no physics change, works best with PhysicsPerFrame=1)
- Fix z-fighting when draw distance is massively increased
- Submersion physics properly applied, can't drive without water physics while under water

Can all be tuned in dethrace.ini

## Potential future improvements

- Buy button in wreck gallery for purchasing cars Carmageddon 2 style
- Achievements - just for fun, who wouldn't want an achievment to pop up when you kill your 10,000th ped 
- Add existing AI as multiplayer mode bots

## Configuation

The following new options are availble under the `[Slop]` section in dethrace.ini

```ini
[Slop]
; Custom resolution, aspect ratio is based on this, for 4:3 choose a 4:3 resolution (suggest window mode if you do this since your monitor probably won't support it natively)
Width=3840
Height=2160

; Enable anti-aliasing
Msaa=8
; Enable Sample Rate Shading - upgrades MSAA to SSAA giving much nicer results at the expense of performance 
SampleRateShading = 1 

; Fixes camera juddering when on slopes (rendering only change, does not affect car physics)
CameraJudderFix=1

; Set YonFactor to 1000 (already possible in options.txt) but also increases draw distances of the following
; - Pedestrians
; - 3d objects e.g. traffic lights/road signs
; - Opponents
; - Powerups
ExtendDrawDistance=1

; Allow stealing any car regardless of type (default 0)
StealworthyAllCars=1
; Probability (0-100) that a car can be stolen when StealworthyAllCars is enabled (default 50)
StealworthyPercentage=50
; Disable the rank gate that normally prevents stealing cars above your rank (default 0)
StealworthyRankLimitDisable=1

; Maximum number of skid/blood/oil decals before old ones are recycled (default 100, max 65535)
NumSkids=65535
```

## Credits

This builds on the incredible [Dethrace](https://github.com/dethrace-labs/dethrace) project 

