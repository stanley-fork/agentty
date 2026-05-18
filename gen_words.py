#!/usr/bin/env python3
"""Generate the 1000-word pool header for agentty's activity indicator.

Lives in the host (agentty) tree, not maya — the widget is content-
agnostic and the host passes the word list in via Config::words.
"""

pools = {
'thinking':'thinking pondering musing reasoning weighing composing reflecting deliberating considering ruminating contemplating analyzing examining assessing evaluating processing computing calculating reckoning deducing inferring concluding synthesizing parsing scanning probing inspecting surveying scrutinizing studying investigating exploring searching seeking hunting tracking tracing pursuing chasing following sniffing prying digging poking peeking glancing observing watching noting marking spotting catching grasping seizing capturing gleaning gathering collecting amassing assembling compiling cataloging indexing sorting filtering ranking ordering grouping classifying tagging labeling naming defining describing depicting framing modeling sketching outlining drafting plotting mapping charting graphing diagramming illustrating rendering visualizing imagining envisioning dreaming wondering surmising guessing speculating hypothesizing theorizing conjecturing postulating positing proposing suggesting offering volunteering advancing presenting submitting tendering proffering hatching brewing cooking baking simmering steeping marinating reducing distilling refining polishing honing tuning tweaking adjusting calibrating aligning balancing leveling fitting matching pairing coupling joining linking chaining bridging knitting weaving stitching binding braiding twining spinning whirling cycling rotating revolving orbiting circling looping iterating recursing branching forking merging joining diverging converging blending mixing folding kneading shaping forging tempering casting molding sculpting carving etching engraving inscribing scribing scribbling jotting noting drafting penning typing keying clacking tapping toggling flipping flicking nudging shifting moving sliding gliding drifting floating soaring hovering meditating dwelling resting pausing breathing easing sinking settling steadying centering grounding rooting anchoring',
'cs':'compiling linking parsing lexing tokenizing minifying bundling transpiling injecting hooking polling waiting blocking yielding awaiting deferring suspending resuming spawning forking joining locking unlocking acquiring releasing draining flushing buffering caching paging swapping unmapping faulting trapping signaling raising catching throwing returning unwinding bubbling exploding logging tracing profiling instrumenting benchmarking timing sampling slicing dicing chunking batching pipelining streaming queuing dequeuing pushing popping peeking enqueuing dispatching routing forwarding bouncing relaying tunneling proxying mediating brokering negotiating handshaking authing signing verifying hashing salting digesting encoding decoding zipping unzipping deflating inflating gzipping checksumming xoring shifting masking clearing setting flipping incrementing decrementing adding subtracting multiplying dividing absorbing accumulating averaging summing scaling normalizing standardizing whitening smoothing denoising sharpening blurring dithering bisecting halving doubling tripling pivoting heapifying balancing rebalancing splaying threading climbing descending traversing walking visiting marking sweeping compacting promoting demoting evicting pinning unpinning prefetching predicting mispredicting speculating retiring committing aborting retrying throttling debouncing coalescing deduplicating uniquifying canonicalizing serializing deserializing marshaling unmarshaling pickling unpickling stringifying flattening hydrating dehydrating memoizing recompiling rebuilding relinking restarting reloading respawning resharding migrating snapshotting checkpointing journaling replicating quorum gossiping heartbeating reaping zombifying daemonizing forking execing chrooting jailing namespacing capping sandboxing',
'math':'integrating differentiating summing telescoping factoring expanding distributing simplifying canceling perturbing bounding clamping interpolating extrapolating quantizing rounding flooring truncating squaring cubing exponenting tangenting secanting cotangenting modding currying uncurrying lifting lowering flatmapping foldmapping warping bending twisting shearing reflecting translating skewing projecting fibering bundling sectioning quotienting unioning intersecting complementing compactifying limiting bordering edging densifying nullifying perfecting gradient diverging curling laplacing fourierizing legendreizing convolving correlating filtering eigening diagonalizing triangulating decomposing orthogonalizing affining homing hilberting metricizing',
'phil':'wondering recollecting remembering forgetting noticing realizing recognizing acknowledging accepting embracing welcoming releasing letting witnessing attending settling listening hearing tasting savoring smelling touching feeling sensing dwelling abiding hoping daring trusting yielding flowing aligning becoming arising emerging unfolding blossoming ripening discerning intuiting apprehending grasping seeing watching pausing breathing inhaling exhaling sighing yawning blinking dozing waking opening closing facing turning bowing kneeling stretching reaching extending withdrawing receding approaching departing dissolving resolving',
'craft':'storyboarding wireframing prototyping iterating tweaking polishing sanding planing chiseling whittling sculpting molding casting forging hammering tempering quenching annealing inking painting glazing firing kilning crocheting sewing stitching darning hemming basting tailoring trimming pressing pleating ruching gathering ruffling beading embroidering quilting felting fulling carding weaving warping wefting plying twisting joinery dovetailing mortising tenoning rabbeting routing scribing measuring leveling plumbing squaring kerfing ripping crosscutting biscuiting doweling clamping gluing',
'kitchen':'simmering reducing stewing braising poaching steaming sauteing searing browning caramelizing deglazing emulsifying whipping proofing rising basting roasting grilling broiling smoking curing pickling fermenting distilling steeping marinating brining sprinkling drizzling tossing plating chopping mincing dicing julienning chiffonading butchering deboning filleting blanching parboiling refreshing tempering glazing seasoning salting peppering zesting muddling pressing juicing extracting infusing shaking stirring blending pureeing whisking sifting folding piping decorating garnishing',
'nature':'flowing drifting rippling cascading meandering pooling welling brimming spilling overflowing ebbing receding cresting peaking breaking foaming churning rolling tumbling swirling eddying whirling spiraling spinning blooming budding sprouting rooting branching leafing fruiting ripening seeding scattering soaring gliding circling perching nesting roosting migrating tracking foraging grazing browsing wandering roaming pollinating burrowing hibernating shedding molting weathering eroding sedimenting crystallizing condensing evaporating sublimating freezing thawing melting',
'music':'tuning practicing rehearsing improvising jamming arranging composing transposing transcribing notating scoring conducting accompanying harmonizing modulating phrasing breathing bowing strumming plucking picking fingering tonguing slurring legato staccato accenting syncopating swinging grooving pulsing throbbing humming whistling singing chanting crooning warbling trilling tremoloing vibratoing glissandoing crescendoing decrescendoing diminuendoing',
'physics':'oscillating resonating damping radiating diffracting refracting absorbing emitting scattering polarizing magnetizing electrifying ionizing nucleating decaying tunneling entangling collapsing measuring thermalizing cooling heating boiling condensing precipitating annealing deforming straining stressing yielding fracturing buckling vibrating accelerating decelerating orbiting freefalling propagating diffusing convecting advecting fluxing',
'writing':'drafting rewriting revising editing redlining proofing copyediting outlining brainstorming freewriting journaling annotating footnoting endnoting citing quoting paraphrasing summarizing abstracting synopsing dictating proofreading galleying typesetting kerning leading hyphenating justifying paginating folioing margining indenting bulleting numbering captioning headlining subheading',
'sport':'training drilling stretching warming cooling jogging sprinting striding pacing leaping bounding hopping skipping vaulting tumbling diving plunging swimming paddling stroking kicking pedaling cycling rowing sculling skating skiing snowboarding surfing climbing belaying rappelling bouldering scrambling hiking trekking trailing scouting orienteering navigating compassing',
'detective':'investigating profiling tailing surveilling shadowing wiretapping bugging swabbing fingerprinting dusting bagging tagging interrogating interviewing questioning grilling badgering pressing baiting ensnaring trapping cornering apprehending arresting cuffing booking',
'ai':'pretraining finetuning aligning prompting fewshotting zeroshotting reranking embedding tokenizing detokenizing chunking sharding distilling pruning quantizing dequantizing checkpointing ablating perturbing attending crossattending selfattending masking unmasking softmaxing argmaxing topking sampling beaming probing teacherforcing rollouting bootstrapping curriculum scheduling annealing warming',
'space':'navigating orbiting docking undocking decoupling separating deploying jettisoning thrusting venting firing igniting circularizing apoapsing periapsing rendezvousing recovering splashing reentering reusing recycling repressurizing pressurizing depressurizing telemetering downlinking uplinking',
'gaming':'levelling crafting harvesting mining smelting enchanting alchemizing buffing debuffing healing tanking dpsing kiting pulling aggroing nuking critting parrying dodging blocking countering riposting feinting comboing juggling cancelling pixelhunting',
'medical':'auscultating palpating percussing diagnosing prescribing dispensing administering injecting infusing transfusing intubating extubating resuscitating defibrillating suturing dressing splinting plastering bandaging scrubbing prepping draping sterilizing autoclaving cauterizing ligating incising excising biopsying scoping endoscoping radiographing imaging',
'finance':'auditing reconciling posting journalling debiting crediting amortizing depreciating accruing deferring hedging shorting longing arbitraging straddling stripping rebalancing diversifying allocating overweighting underweighting screening backtesting walkforwarding portfolioing',
'astro':'galaxying nebulating constellating starforming supernovaing collapsing accreting jetting beaming eclipsing transiting occulting microlensing dopplering redshifting blueshifting parallaxing spectroscoping photometrizing astrometrizing interferometering',
'bio':'translating transcribing splicing replicating mitosing meiosing differentiating apoptosing phagocytosing endocytosing exocytosing osmosing pumping channeling cascading phosphorylating dephosphorylating methylating demethylating acetylating ubiquitinating misfolding chaperoning escorting',
'travel':'departing arriving boarding deplaning ticketing checking gating queueing immigrating emigrating transiting layovering connecting rerouting cancelling rescheduling overbooking overnighting backpacking hostelling couchsurfing',
'misc':'orienting reorienting situating contextualizing decontextualizing recontextualizing problematizing thematizing dramatizing romanticizing systematizing prioritizing reprioritizing deprioritizing optimizing pessimizing maximizing minimizing satisficing heuristicing mnemonicizing',
}

pad = ('reaffirming reformulating reframing rephrasing rewording revisiting '
       'reassessing rethinking reweighing reconsidering rechecking restating '
       'reordering reorganizing replanning relighting rekindling renewing '
       'repairing rehabilitating recalibrating reconfiguring redesigning '
       'recapping recasting reapplying reabsorbing reactivating readapting '
       'readdressing readjusting readying realigning reanalyzing reanimating '
       'reannouncing reappearing reapportioning reappraising rearming '
       'rearranging reassembling reassigning reassuring reattaching '
       'reattempting reauthorizing reawakening rebranding canting capping '
       'receding recharging reclaiming reclassifying recleaning reclothing '
       'recoiling recombining recommencing recommending recompensing '
       'recomposing reconciling reconditioning reconnecting reconnoitering '
       'reconquering reconstituting reconstructing recontacting reconverting '
       'recooking recopying recording recounting recoupling recovering '
       'recreating recrossing recrowning recruiting rectifying recuperating '
       'recurring recycling rededicating redefining redelivering redepositing '
       'redeveloping redirecting rediscovering redistributing reediting '
       'reeducating reelecting reembarking reembracing reemerging reemitting '
       'reemploying reenacting reencountering reendorsing reenergizing '
       'reenforcing reengaging reenlisting reenrolling reentering reequipping '
       'reerecting reescalating reestablishing reestimating reevaluating '
       'reexamining reexecuting reexploring reexporting reexpressing refacing '
       'refalling refashioning refastening refencing refereeing refertilizing '
       'refilling refilming refinancing refixing reflagging refloating '
       'refluxing refocusing reforesting reforging reformatting refortifying '
       'refractoring refracturing refreezing refurbishing regaining '
       'regathering regenerating reglazing regraduating regrafting regrading '
       'regrowing reguarding regularizing rehashing rehearsing reheating '
       'rehiring rehoming rehosting rehydrating reigniting reilluminating '
       'reimagining reimbursing reimplanting reimporting reimposing '
       'reincarnating reinflating reinforcing reinitializing reinjecting '
       'reinjuring reinscribing reinserting reinspecting reinstalling '
       'reinstating reinstigating reinsuring reintegrating reintroducing '
       'reinventing reinvesting reinvigorating reinvolving reissuing '
       'reiterating rejecting rejoicing rejoining rejudging rejuvenating '
       'rekeying releasing relegating relenting relettering releveling '
       'relieving relighting relisting reliving reloading relocating '
       'remaining remaking remanding remapping remarketing remastering '
       'remediating remembering remilitarizing reminding reminiscing '
       'remitting remodeling remodulating remoistening remonstrating '
       'remortgaging remounting removing renaming rendering renegotiating '
       'renominating renotifying renouncing renovating renumbering '
       'reoccupying reoccurring reoffering reopening reordering reorienting '
       'repackaging repacking repairing repapering reparking repartitioning '
       'repasting repatching repatriating repaving repaying repealing '
       'repeating repelling repenting repeopling reperforming repermitting '
       'repetitioning rephotographing rephrasing repining repinning '
       'replacing replanning replanting replastering replaying replenishing '
       'replicating replying repolishing repolling repopulating reporting '
       'repositing repositioning repossessing repotting repouring repreparing '
       'repressing reprieving reprinting reprising reprocessing reproducing '
       'reprogramming reproving repulsing repurchasing repurifying '
       'repurposing requesting requiring requisitioning resaddling resailing '
       'resampling resawing rescaling rescanning rescheduling rescinding '
       'rescuing researching reselecting reseparating reservicing resetting '
       'resettling reshaping resharpening reshelving reshipping reshoeing '
       'reshoring reshowing reshuffling residing resignaling resignifying '
       'resigning resinging resizing resmoothing resoaking resocializing '
       'resoldering resoling resorting resounding resourcing respacing '
       'respeaking respecifying respecting respelling respiring resplitting '
       'responding restacking restaffing restamping restaping restating '
       'restepping resterilizing restimulating restitching restocking '
       'restoking restoring restraining restricting restringing restructuring '
       'restudying restuffing restyling resubmitting resulting resuming '
       'resurfacing resurveying resurviving resuscitating retabulating '
       'retagging retailoring retaking retallying retasking retasting '
       'reteaching retelling rethinking rethreading retightening retiling '
       'retiming retiring retoning retooling retouching retracing retracking '
       'retraining retransferring retransmitting retreading retreating '
       'retrenching retrieving retrofitting retrying returning retuning '
       'retwisting retying reunifying reupholstering reusing reuttering '
       'revaccinating revaluing revamping revealing reveling reverberating '
       'revering reversing reverting revictualing reviewing reviling '
       'revising revisiting reviving revoking revolting rewashing rewatching '
       'rewearing reweaving reweighing rewelding rewetting rewinding '
       'rewinning rewiring rewording reworking rewrapping rewriting rezoning')

words = []
seen = set()
for v in pools.values():
    for w in v.split():
        w = w.strip().lower()
        if 3 <= len(w) <= 12 and w.isalpha() and w not in seen:
            seen.add(w); words.append(w)

if len(words) < 1000:
    for w in pad.split():
        w = w.strip().lower()
        if 3 <= len(w) <= 12 and w.isalpha() and w not in seen:
            seen.add(w); words.append(w)

target = 1000
words = words[:target]
assert len(words) == target, f'have {len(words)}'

out = '/home/ayush/projects/moha/include/agentty/runtime/view/thread/activity_indicator_words.hpp'
with open(out, 'w') as f:
    f.write('#pragma once\n')
    f.write('// agentty activity-indicator word pool \u2014 1000 curated terms.\n')
    f.write('// Auto-generated from gen_words.py; checked in. Owned by the host so\n')
    f.write("// maya's ActivityIndicator widget stays content-agnostic \u2014 the host\n")
    f.write('// passes the rotating word list into Config::words.\n')
    f.write('//\n')
    f.write('// Mixed domains so the verb stream feels alive: thinking/reasoning,\n')
    f.write('// CS, math, philosophy, craft, kitchen, nature, music, physics,\n')
    f.write('// writing, sport, detective, AI, space, gaming, medical, finance,\n')
    f.write('// astro, bio, travel, plus a tail of re-prefix verbs. All entries\n')
    f.write('// are lowercase ASCII, 3-12 chars.\n\n')
    f.write('#include <array>\n#include <string_view>\n\n')
    f.write('namespace agentty::ui::indicator_words {\n\n')
    f.write(f'inline constexpr std::array<std::string_view, {len(words)}> kPool{{\n')
    cols = 5
    for i, w in enumerate(words):
        if i % cols == 0: f.write('    ')
        pad_spaces = 14 - len(w) - 3
        if pad_spaces < 1: pad_spaces = 1
        f.write(f'"{w}",' + ' ' * pad_spaces)
        if i % cols == cols - 1: f.write('\n')
    if len(words) % cols != 0: f.write('\n')
    f.write('};\n\n} // namespace agentty::ui::indicator_words\n')

print(f'wrote {out} with {len(words)} entries')
