# Tests sur breadboard

Validation progressive, un bloc à la fois. Alimentation : LiPo.

| Étape | Test | Résultat | Statut |
|---|---|---|---|
| 1 | PIR seul : temps de stabilisation à la mise sous tension | ≈ 1 s | ✅ |
| 1 | PIR seul : durée de l'impulsion pour un passage de main | ≈ 2 s | ✅ |
| 1 | PIR seul : redéclenchement en mouvement continu | Sortie maintenue haute | ✅ |
| 2 | Bascule 1 : SET / RESET manuels | Conforme à la table de vérité | ✅ |
| 2 | Bascule 1 : SET = RESET = 1 | Q = 1 (état interdit, conforme datasheet) | ✅ |
| 2 | Bascule 1 : mémorisation du passage (PIR sur CLK1) | LED témoin reste allumée | ✅ |
| 3 | Bascule 2 : blocage (SET2) puis mouvements répétés | Aucun réveil pendant le blocage | ✅ |
| 3 | Bascule 2 : réarmement (RESET2) | Le PIR rallume la LED | ✅ |
| 4 | Q1 (2N7000) : tension de drain, Q1 = 1, pull-up 2,2 kΩ | À mesurer | ⏳ |
| 5 | Q2 (IRLZ44N) + LED IR | À mesurer | ⏳ |
| 6 | TimerCam seule : extinction et réveil par la RTC | À tester | ⏳ |
| 7–8 | Système complet sur batterie | À tester | ⏳ |

## Problèmes rencontrés et solutions

| Problème | Cause | Solution |
|---|---|---|
| Le reset ne fonctionnait pas, SET1 mesuré à 4 V | Fil de SET1 relié au rail + | SET1 ramené à la masse ; SET = RESET = 1 donne Q = 1 |
| Q1 à 3,25 V malgré RESET1 = 1 | CD4013 en miroir par rapport au plan (broche 1 en 13e et non 13f) | Repérage de la broche 1 sur la datasheet, inversion des moitiés a-e / f-j |
| Le blocage n'éteignait pas la LED | SET1 resté à 1 après le test manuel | SET1 définitivement à la masse |
