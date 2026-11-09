/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * golden_lang.h - fixed per-language fixtures and profile templates for the
 * golden corpus tool.  All strings are literals: no generation, no RNG.
 */
#ifndef GOLDEN_LANG_H
#define GOLDEN_LANG_H

typedef struct {
    const char *code;      /* language code string                  */
    const char *instruction;
    const char *opt0;
    const char *opt1;
    const char *statement; /* noul statement in the same language    */
    const char *sentence;  /* decisive sentence embedded in contexts */
} glang;

/* Eleven concrete languages (auto uses the mixed-script fixture).  These
 * sentences are FIXED test fixtures, not claims about language quality. */
static const glang G_LANGS[] = {
    {"und", "Which plan should we pick?", "annual plan", "monthly plan",
     "The annual plan saves twenty percent.",
     "The team compared the annual plan with the monthly plan and picked "
     "the annual plan because it saves twenty percent."},
    {"en", "Which plan should we pick?", "annual plan", "monthly plan",
     "The annual plan saves twenty percent.",
     "The team compared the annual plan with the monthly plan and picked "
     "the annual plan because it saves twenty percent."},
    {"zh", "应该选择哪个方案？", "年度方案", "月度方案",
     "年度方案节省百分之二十。",
     "团队比较了年度方案和月度方案，最终选择了年度方案，因为它节省百分之二十。"},
    {"hi", "हमें कौन सी योजना चुननी चाहिए?", "वार्षिक योजना", "मासिक योजना",
     "वार्षिक योजना में बीस प्रतिशत की बचत है।",
     "टीम ने वार्षिक योजना और मासिक योजना की तुलना की और वार्षिक योजना चुनी "
     "क्योंकि इसमें बीस प्रतिशत की बचत है।"},
    {"es", "¿Qué plan deberíamos elegir?", "plan anual", "plan mensual",
     "El plan anual ahorra veinte por ciento.",
     "El equipo comparó el plan anual con el plan mensual y eligió el plan "
     "anual porque ahorra veinte por ciento."},
    {"ar", "أي خطة يجب أن نختار؟", "الخطة السنوية", "الخطة الشهرية",
     "الخطة السنوية توفر عشرين بالمئة.",
     "قارن الفريق بين الخطة السنوية والخطة الشهرية واختار الخطة السنوية "
     "لأنها توفر عشرين بالمئة."},
    {"fr", "Quel plan devrions-nous choisir ?", "forfait annuel",
     "forfait mensuel",
     "Le forfait annuel économise vingt pour cent.",
     "L'équipe a comparé le forfait annuel et le forfait mensuel, puis a "
     "choisi le forfait annuel car il économise vingt pour cent."},
    {"bn", "আমাদের কোন পরিকল্পনা বেছে নেওয়া উচিত?", "বার্ষিক পরিকল্পনা",
     "মাসিক পরিকল্পনা",
     "বার্ষিক পরিকল্পনায় শতকরা বিশ সাশ্রয় হয়।",
     "টিম বার্ষিক পরিকল্পনা এবং মাসিক পরিকল্পনা তুলনা করে বার্ষিক "
     "পরিকল্পনা বেছে নিয়েছে কারণ এতে শতকরা বিশ সাশ্রয় হয়।"},
    {"pt", "Qual plano devemos escolher?", "plano anual", "plano mensal",
     "O plano anual economiza vinte por cento.",
     "A equipe comparou o plano anual com o plano mensal e escolheu o "
     "plano anual porque economiza vinte por cento."},
    {"id", "Paket mana yang harus kita pilih?", "paket tahunan",
     "paket bulanan",
     "Paket tahunan menghemat dua puluh persen.",
     "Tim membandingkan paket tahunan dengan paket bulanan dan memilih "
     "paket tahunan karena menghemat dua puluh persen."},
    {"ur", "ہمیں کس پلان کا انتخاب کرنا چاہیے؟", "سالانہ پلان", "ماہانہ پلان",
     "سالانہ پلان میں بیس فیصد بچت ہے۔",
     "ٹیم نے سالانہ پلان اور ماہانہ پلان کا موازنہ کیا اور سالانہ پلان "
     "منتخب کیا کیونکہ اس میں بیس فیصد بچت ہے۔"}
};
#define G_LANG_COUNT (sizeof(G_LANGS) / sizeof(G_LANGS[0]))

/* Fixed mixed-script fixture for the AUTO detector (English + Chinese). */
static const char *G_AUTO_SENTENCE =
    "Support summary: the user picked the annual plan. "
    "理由：年度方案每年节省百分之二十。";
static const char *G_AUTO_INSTRUCTION = "Which plan did the user pick? 你?";
static const char *G_AUTO_OPTS[2] = {"annual plan", "monthly plan"};

#endif /* GOLDEN_LANG_H */
