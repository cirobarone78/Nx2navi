// Co-op puppets (coop_puppet.c): the actors the other players are shown as, made at start from the game's own files
// while co-op is on (NX2NAVI_COOP). A puppet is the pause menu's Link (PouchPlayer_Child / PouchPlayer_Adult: Link's
// model, cloth and equipment without the player's control, camera or physics) under a name of its own, with Link's
// world animation set (his AS and its settings) in place of the menu's, and without the menu's logic (its AI asks for
// the current menu and turns and hides the actor for it):
//   - Pack/Actor/CoopPuppet_<age>.pack.zs, a copy of the pause menu Link's pack, with its ActorParam copied under the
//     puppet's name (the create of actor N reads Actor/N.engine__actor__ActorParam.bgyml) and edited there: ASRef to
//     Link's AS, ASInfoRef to Link's AS settings (copied in from Link's own pack), AIInfoRef blanked ("" is how the
//     game's own ActorParams leave a component out);
//   - a row CoopPuppet_<age> in ActorInfo, a copy of the pause menu Link's with the instance heap of Link's own (his AS
//     takes far more than the menu's), and one in GameActorInfo.
#include "restore.h"

#define CHILD "Pack/Actor/CoopPuppet_Child.pack.zs"
#define ADULT "Pack/Actor/CoopPuppet_Adult.pack.zs"
#define CHILD_PARAM "Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"
#define ADULT_PARAM "Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"
#define ACTOR_INFO "RSDB/ActorInfo.Product.111.rstbl.byml.zs"
#define GAME_ACTOR_INFO "RSDB/GameActorInfo.Product.111.rstbl.byml.zs"
#define AS_INFO_PLAYER "Component/ASInfo/Player.engine__component__ASInfo.bgyml"
#define AS_INFO_CHILD "Component/ASInfo/Player_Child.engine__component__ASInfo.bgyml"

static const RestoreField child_row[] = {{"InstanceHeapSize", 5692448, NULL}};    // Player_Child's
static const RestoreField adult_row[] = {{"InstanceHeapSize", 10245064, NULL}};   // Player_Adult's

static const RestoreEdit edits[] = {
    RESTORE_NEW_FILE(CHILD, "Pack/Actor/PouchPlayer_Child.pack.zs"),
    RESTORE_COPY_MEMBER(CHILD, CHILD_PARAM, NULL, "Actor/PouchPlayer_Child.engine__actor__ActorParam.bgyml"),
    RESTORE_COPY_MEMBER(CHILD, AS_INFO_CHILD, "Pack/Actor/Player_Child.pack.zs", AS_INFO_CHILD),
    RESTORE_COPY_MEMBER(CHILD, AS_INFO_PLAYER, "Pack/Actor/Player_Child.pack.zs", AS_INFO_PLAYER),
    RESTORE_SET_MEMBER_TEXT(CHILD, CHILD_PARAM, "Components/ASRef", "?AS/Player_Pouch.root.asb", "?AS/Player_Child.root.asb"),
    RESTORE_SET_MEMBER_TEXT(CHILD, CHILD_PARAM, "Components/ASInfoRef",
                            "?Component/ASInfo/PouchPlayer.engine__component__ASInfo.bgyml", "?" AS_INFO_CHILD),
    RESTORE_SET_MEMBER_TEXT(CHILD, CHILD_PARAM, "Components/AIInfoRef", "?AI/AIInfo/PouchPlayer.engine__actor__AIInfo.bgyml", ""),
    RESTORE_ADD_ROW(ACTOR_INFO, NULL, NULL, "__RowId", "PouchPlayer_Child", "CoopPuppet_Child", child_row),
    RESTORE_ADD_ROW_AS(GAME_ACTOR_INFO, NULL, NULL, "__RowId", "PouchPlayer_Child", "CoopPuppet_Child"),

    RESTORE_NEW_FILE(ADULT, "Pack/Actor/PouchPlayer_Adult.pack.zs"),
    RESTORE_COPY_MEMBER(ADULT, ADULT_PARAM, NULL, "Actor/PouchPlayer_Adult.engine__actor__ActorParam.bgyml"),
    RESTORE_COPY_MEMBER(ADULT, AS_INFO_PLAYER, "Pack/Actor/Player_Adult.pack.zs", AS_INFO_PLAYER),
    RESTORE_SET_MEMBER_TEXT(ADULT, ADULT_PARAM, "Components/ASRef", "?AS/Player_Pouch.root.asb", "?AS/Player_Adult.root.asb"),
    RESTORE_SET_MEMBER_TEXT(ADULT, ADULT_PARAM, "Components/ASInfoRef",
                            "?Component/ASInfo/PouchPlayer.engine__component__ASInfo.bgyml", "?" AS_INFO_PLAYER),
    RESTORE_SET_MEMBER_TEXT(ADULT, ADULT_PARAM, "Components/AIInfoRef", "?AI/AIInfo/PouchPlayer.engine__actor__AIInfo.bgyml", ""),
    RESTORE_ADD_ROW(ACTOR_INFO, NULL, NULL, "__RowId", "PouchPlayer_Adult", "CoopPuppet_Adult", adult_row),
    RESTORE_ADD_ROW_AS(GAME_ACTOR_INFO, NULL, NULL, "__RowId", "PouchPlayer_Adult", "CoopPuppet_Adult"),
};

RESTORATION_FOR(coop_puppet, "COOP_PUPPET", "NX2NAVI_COOP", "Co-op puppets",
                "The actors other co-op players are shown as: the pause menu's Link with Link's world animations.", edits);
