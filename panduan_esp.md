‎This Tutorial Is Made For Simple Esp Tutorial With Lgl Mod Menu.

‎*Conditions
‎
‎1. YOU Must Have Esp Overlay Added In Your Project.
‎2. You must Have Knowledge About Screen, Vectors and More.
‎3. You Must Not Be Beginner.
‎
‎So, How We Going To make esp.
‎
‎
‎Simple Steps:
‎
‎1. Get Entity List
‎2. Get Entity Position
‎3. Filter With World To Screen Point
‎4. Draw
‎Start:
‎We Can Use Simple EspManager Class For Managing The Entity:
‎
‎Create A Header File With ESPManager.h name
‎and Import it into project
‎
‎
‎C++:
‎void *myPlayer = NULL;
‎struct enemy_t {
‎    void *object;
‎    Vector3 location;
‎    int health;
‎
‎};
‎class ESPManager {
‎public:
‎    std::vector<enemy_t *> *enemies;
‎
‎    ESPManager() {
‎        enemies = new std::vector<enemy_t *>();
‎    }
‎    bool isEnemyPresent(void *enemyObject) {
‎        for (std::vector<enemy_t *>::iterator it = enemies->begin(); it != enemies->end(); it++) {
‎            if ((*it)->object == enemyObject) {
‎                return true;
‎            }
‎        }
‎        return false;
‎    }
‎    void removeEnemy(enemy_t *enemy) {
‎        for (int i = 0; i < enemies->size(); i++) {
‎            if ((*enemies)[i] == enemy) {
‎                enemies->erase(enemies->begin() + i);
‎                return;
‎            }
‎        }
‎    }
‎    void tryAddEnemy(void *enemyObject) {
‎        if (isEnemyPresent(enemyObject)) {
‎            return;
‎        }
‎
‎        if (IsPlayerDead(enemyObject)) {
‎            return;
‎        }
‎
‎        enemy_t *newEnemy = new enemy_t();
‎
‎        newEnemy->object = enemyObject;
‎
‎        enemies->push_back(newEnemy);
‎    }
‎    void updateEnemies(void *enemyObject) {
‎        for (int i = 0; i < enemies->size(); i++) {
‎            enemy_t *current = (*enemies)[i];
‎
‎            if (IsPlayerDead(current->object)) {
‎                enemies->erase(enemies->begin() + i);
‎            }
‎        }
‎    }
‎    void removeEnemyGivenObject(void *enemyObject) {
‎        for (int i = 0; i < enemies->size(); i++) {
‎            if ((*enemies)[i]->object == enemyObject) {
‎                enemies->erase(enemies->begin() + i);
‎                return;
‎            }
‎        }
‎    }
‎
‎};
‎
‎* This ESPManager Is Not Coded By Me, The Credit Goes To Respective Programmer
‎
‎Now, Lets Setup this To Our project
‎Code:
‎//Include Header File
‎#include "ESPManager.h"
‎
‎Now, Create A Variable of ESPManager
‎
‎Screenshot_20220704-215359.png
‎
‎
‎Now We Need To initialize espManager
‎
‎Initialize it into Main Thread
‎Screenshot_20220704-215618.png
‎C++:
‎espManager = new ESPManager();
‎this code was used to initialize
‎
‎
‎For this Tutorial, I am Using Dead Target Game
‎
‎First, we Must know where we are going to draw esp.
‎For my Game I Am Going to Draw Esp To zombies.
‎
‎Getting Entity List.
‎Now, We Need To Find the Correct class,
‎In my Case, its ZombieBase
‎
‎Screenshot_20220704-213808.png
‎
‎
‎Now, its The Base Of Zombie.
‎
‎Now, We Need To find Update () or Lateupdate () or Any update Method Is This Class.
‎I found The Correct Lateupdate Method:
‎Screenshot_20220704-214008.png
‎
‎
‎Now, We Need to Hook Lateupdate, So We Can Get The Data From ZombieBase Class
‎
‎So,
‎I Hooked Like This:
‎C++:
‎void *enemyPlayer = NULL;
‎void (*update)(void *player);
‎void _update(void *player) {
‎    if (player != NULL) {
‎        update(player);
‎        enemyPlayer = player;
‎    }
‎    if (IsEsp) {
‎        if (enemyPlayer) {
‎            espManager->tryAddEnemy(player);
‎        }
‎    }
‎}
‎
‎//HOOK CODE
‎
‎MSHookFunction((void *) getAbsoluteAddress(libName, 0x861E20), (void *) &_update, (void **) &update);
‎
‎
‎Now, If Your Class Have Destory, or Dead Method You can also hook this,
‎like that
‎C++:
‎void (*destroy)(void *player);
‎
‎void _destroy(void *player) {
‎
‎    if (player != NULL) {
‎
‎        Player_update(player);
‎     
‎        espManager->removeEnemyGivenObject(player);
‎
‎    }
‎
‎}
‎
‎
‎//HOOK CODE
‎
‎MSHookFunction((void *) getAbsoluteAddress(libName, 0xOffsets), (void *) &_destroy, (void **) &destroy);
‎If your game dont have ondestroy, you dont need to add this one.
‎
‎Now, All our Player Objects Will be Stored In EspManager Array.
‎
‎so Now We Can Count.
‎In Other Word, We Got EntityList.
‎
‎Getting Tranform of Enemy
‎Now,
‎We Can Get Tranform Of That Object Easily,
‎To do this, We Need to Find Component Class.
‎C++:
‎public class Component : Object
‎
‎Now Inside This class, Find
‎Code:
‎public Transform get_transform() { }
‎Screenshot_20220704-221723.png
‎
‎We Need A Hook:
‎
‎For Getting Transform From The Object;
‎C++:
‎void *getTransform(void *player) {
‎    if (!player) return NULL;
‎    static const auto get_transform_injected = reinterpret_cast<uint64_t(__fastcall *)(void *)>(getAbsoluteAddress("libil2cpp.so", 0x1D262C8));
‎    return (void *) get_transform_injected(player);
‎}
‎
‎Now, we Can Get The Tranform From The Object.
‎For That, We Can Use:
‎Code:
‎void *playerTF = getTransform(object);
‎
‎Getting Position From Transform
‎Now We Need To get Position in vector3 Of This Transform,
‎
‎so, To do so, we Need Find A Class
‎C++:
‎public class Transform : Component, IEnumerable
‎
‎Now, We Need To Find this Method In that Class
‎C++:
‎public Vector3 get_position() { }
‎Screenshot_20220704-223145.png
‎
‎
‎Now, We Need To Hook This Method:
‎
‎I hooked Like This:
‎C++:
‎Vector3 get_position(void *transform) {
‎    if (!transform)return Vector3();
‎    Vector3 position;
‎    static const auto get_position_injected = reinterpret_cast<uint64_t(__fastcall *)(void *,Vector3 &)>(getAbsoluteAddress("libil2cpp.so", 0x1E95024));
‎    get_position_injected(transform, position);
‎    return position;
‎}
‎
‎Now, We Can Get Position
‎Code:
‎void *playerTF = getTransform(object);
‎
‎Vector3 PlayerPos = get_position(playerTF);
‎
‎Filtering Through WorldToScreenPoint
‎If We Draw Esp from That Position we got, it will draw esp even if enemy is behind you, or enemy is not visible is screen, So we need to use World To Screen Point.
‎
‎To Use That, Find Class:
‎C++:
‎public sealed class Camera : Behaviour
‎Now, Inside This Class, Find
‎Code:
‎private void WorldToScreenPoint_Injected(ref Vector3 position, Camera.MonoOrStereoscopicEye eye, out Vector3 ret) { }
‎For My Game, its like this:
‎Screenshot_20220704-224717.png
‎
‎
‎
‎Now, We Need To Hook it,
‎The Hook:
‎C++:
‎Vector3 WorldToScreenPoint(void *transform, Vector3 test) {
‎    if (!transform)return Vector3();
‎    Vector3 position;
‎    static const auto WorldToScreenPoint_Injected = reinterpret_cast<uint64_t(__fastcall *)(void *,Vector3, int, Vector3 &)>(getAbsoluteAddress("libil2cpp.so", 0x1D232D4));
‎      WorldToScreenPoint_Injected(transform, test, 4, position);
‎      return position;
‎}
‎
‎To Get WorldToScreenPoint, We Need Main Camera Of This Game
‎For Camera, Find Class:
‎C++:
‎public sealed class Camera : Behaviour
‎Now, Inside This Class, Find
‎Code:
‎public static Camera get_main() { }
‎For My Game, its like this:
‎Screenshot_20220704-225641.png
‎
‎We Need To Hook This Also,
‎
‎The Hook:
‎C++:
‎void *get_camera() {
‎    static const auto get_camera_injected = reinterpret_cast<uint64_t(__fastcall *)()>(getAbsoluteAddress("libil2cpp.so", 0x1D23AD4));
‎    return (void *) get_camera_injected();
‎}
‎
‎Now, We Can Get Filtered Position like that:
‎C++:
‎void *playerTF = getTransform(object);
‎
‎Vector3 PlayerPos = get_position(playerTF);
‎
‎void *cam = get_camera();
‎
‎Vector3 Position = WorldToScreenPoint(cam, PlayerPos);
‎
‎So, Getting Position Is Done,
‎
‎
‎Now We Can Get Position,
‎We can create A Method to get position
‎C++:
‎Vector3 GetPlayerLocation(void *player) {
‎    return get_position(getTransform(player));
‎}
‎
‎Getting Enemy Stats
‎We Need To get Enemy Stats to know Enemy Is Dead Or Alive
‎
‎For My Case, I am Getting Zombie Health and if health is equals to 0, enemy is dead
‎
‎My Code:
‎Code:
‎int GetPlayerHealth(void *player) {
‎    if (!player) return NULL;
‎    static const auto get_Health = reinterpret_cast<uint64_t(__fastcall *)(void *)>(getAbsoluteAddress("libil2cpp.so", 0x859AB0));
‎    return (int) get_Health(player);
‎}
‎
‎bool PlayerAlive(void *player) {
‎    return player != NULL && GetPlayerHealth(player) > 0;
‎}
‎
‎bool IsPlayerDead(void *player) {
‎    return player == NULL && GetPlayerHealth(player) < 1;
‎}
‎You can use your own way to get if player is dead or alive, but it is important to Remove esp when enemy is dead
‎Now, We Are Almost Done, Now We Can Draw Esp
‎
‎
‎
‎Drawing Esp
‎Inside Your Draw Method
‎Code:
‎//Check Esp Is Enabled
‎
‎if(IsEsp) {
‎
‎
‎
‎//Code Goes Inside it
‎
‎
‎
‎}
‎Now, If There are No Enemies, We Dont Need to draw
‎so, code
‎C++:
‎if (espManager->enemies->empty()) {
‎            return;
‎}
‎
‎Now We If Enemies Are Visible We Can Get Enemy Count And Can loop
‎
‎Code:
‎for (int i = 0; i < espManager->enemies->size(); i++) {
‎   //Code Goes Here
‎}
‎
‎Now, We Can Get The Players And Check if Alive
‎C++:
‎void *Player = (*espManager->enemies)[i]->object;
‎if (PlayerAlive(Player)) {
‎   //Code Goes Here
‎} else {
‎espManager->removeEnemyGivenObject(Player);
‎}
‎
‎Now, We Can Get The Position Here
‎
‎C++:
‎Vector3 PlayerPos = GetPlayerLocation(Player);
‎void *Cam = get_camera();
‎Vector3 PosNew = WorldToScreenPoint(Cam, PlayerPos);
‎if (PosNew.z < 1.f) continue;
‎//More Code Here
‎
‎
‎Finally We Got The Position, Now We Need to Draw The Esp
‎Drawing Method Maybe Different As Project
‎C++:
‎Vector2 DrawFrom = Vector2(screenWidth / 2, 0);
‎Vector2 DrawTo = Vector2((screenWidth- (screenWidth - PosNew.x)) + 5f, (screenHeight - PosNew.y - 10.0f));
‎if (ESPLine) {          
‎    esp.DrawLine(Color::Red(), 2, DrawFrom, DrawTo);
‎}
‎
‎
‎Now The Draw Function May Look Like This:
‎C++:
‎if(IsEsp) {
‎//Code Goes Inside it
‎    if (espManager->enemies->empty()) {
‎        return;
‎    }
‎    for (int i = 0; i < espManager->enemies->size(); i++) {
‎        //Code Goes Here
‎        void *Player = (*espManager->enemies)[i]->object;
‎        if (PlayerAlive(Player)) {
‎            //Code Goes Here
‎            Vector3 PlayerPos = GetPlayerLocation(Player);
‎            void *Cam = get_camera();
‎            Vector3 PosNew = WorldToScreenPoint(Cam, PlayerPos);
‎            if (PosNew.z < 1.f) continue;
‎            //More Code Here
‎            Vector2 DrawFrom = Vector2(screenWidth / 2, 0);
‎            Vector2 DrawTo = Vector2((screenWidth- (screenWidth - PosNew.x)) + 5f, (screenHeight - PosNew.y - 10.0f));
‎            if (ESPLine) {
‎                esp.DrawLine(Color::Red(), 2, DrawFrom, DrawTo);
‎            }
‎        }
‎    }
‎
‎}
‎By Using The Tutorial Given, My Mod Looks Like This
‎Screenshot_20220704-235700_1.jpg
‎I Am Leaving A Sample Project Here To Make All things Easy To Learn
‎