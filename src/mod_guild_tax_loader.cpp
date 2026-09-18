/*
 * This file is part of mod-guild-tax, a module for AzerothCore, released under the
 * GNU GPL v2 license: https://github.com/xorbis/mod-guild-tax/blob/main/LICENSE
 */

void AddGuildTaxScripts();

// Called by the core's generated module loader; the name follows the folder name.
void Addmod_guild_taxScripts()
{
    AddGuildTaxScripts();
}
